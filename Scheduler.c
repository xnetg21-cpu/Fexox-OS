/*
 * Scheduler.c — CFS-подобный планировщик FEXOS
 *
 * Алгоритм выбора задачи:
 *   - Min-heap по vruntime: всегда берём задачу с наименьшим vruntime.
 *   - vruntime += delta_real * (NICE_0_WEIGHT / task->weight)
 *   - Новая задача получает vruntime = min_vruntime очереди (не отстаёт).
 *   - Квант: SCHED_TIMESLICE_NS (4 мс). По истечению — вытеснение.
 *
 * Переключение контекста:
 *   - Таймерный обработчик (sched_tick) вызывается с указателем на cpu_frame_t
 *     на стеке прерывания. Мы подменяем rsp текущей задачи на этот кадр,
 *     берём следующую задачу и восстанавливаем её rsp.
 *   - iret вернёт управление в нужную точку следующей задачи.
 */

#include "Scheduler.h"
#include "debug_out.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Прямые вызовы для логирования (без kmalloc рекурсии) */
extern void dbg_puts(const char *s);
extern void dbg_hex64(uint64_t v);

/* -------------------------------------------------------------------------
 * Внешние зависимости
 * ------------------------------------------------------------------------- */
extern void  *kmalloc(uint64_t size);
extern void   kfree(void *ptr);
extern void   interrupt_set_scheduler_tick(uint64_t (*fn)(uint64_t frame_rsp));

/* -------------------------------------------------------------------------
 * Spinlock — однопроцессорная реализация через CLI/STI
 * На UP-ядре достаточно маскировать прерывания.
 * Сохраняем/восстанавливаем EFLAGS чтобы sti не вызвал #GP если IF=0.
 * ------------------------------------------------------------------------- */
typedef volatile int spinlock_t;

static inline void spinlock_acquire(spinlock_t *l) {
    (void)l;
    __asm__ volatile ("pushfq\n\tcli\n\tpopfq" ::: "memory");
}

static inline void spinlock_release(spinlock_t *l) {
    (void)l;
    __asm__ volatile ("pushfq\n\tsti\n\tpopfq" ::: "memory");
}

/* TSC для измерения времени */
static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* Простое преобразование TSC -> нс (калибруется при init) */
static uint64_t g_tsc_per_ns = 3; /* ~3 ГГц по умолчанию */

static inline uint64_t tsc_to_ns(uint64_t tsc) {
    return tsc / g_tsc_per_ns;
}

/* -------------------------------------------------------------------------
 * Таблица весов (как в Linux, для nice [-20..19])
 * -------------------------------------------------------------------------
 * Каждый шаг ≈ 1.25x. nice=0 → 1024.
 * ------------------------------------------------------------------------- */
static const uint32_t prio_to_weight[40] = {
    /* -20 */  88761, 71755, 56483, 46273, 36291,
    /* -15 */  29154, 23254, 18705, 14949, 11916,
    /* -10 */   9548,  7620,  6100,  4904,  3906,
    /*  -5 */   3121,  2501,  1991,  1586,  1277,
    /*   0 */   1024,   820,   655,   526,   423,
    /*   5 */    335,   272,   215,   172,   137,
    /*  10 */    110,    87,    70,    56,    45,
    /*  15 */     36,    29,    23,    18,    15,
};

static inline uint32_t prio_to_w(int prio) {
    if (prio < SCHED_PRIO_MIN) prio = SCHED_PRIO_MIN;
    if (prio > SCHED_PRIO_MAX) prio = SCHED_PRIO_MAX;
    return prio_to_weight[prio + 20];
}

/* -------------------------------------------------------------------------
 * Min-heap по vruntime
 * Поддерживает до SCHED_MAX_TASKS элементов.
 * ------------------------------------------------------------------------- */
typedef struct {
    task_t  *data[SCHED_MAX_TASKS];
    int      size;
} run_queue_t;

static inline void heap_swap(run_queue_t *h, int a, int b) {
    task_t *tmp = h->data[a];
    h->data[a] = h->data[b];
    h->data[b] = tmp;
    h->data[a]->heap_idx = a;
    h->data[b]->heap_idx = b;
}

static void heap_up(run_queue_t *h, int i) {
    while (i > 0) {
        int parent = (i - 1) / 2;
        if (h->data[parent]->vruntime <= h->data[i]->vruntime)
            break;
        heap_swap(h, parent, i);
        i = parent;
    }
}

static void heap_down(run_queue_t *h, int i) {
    while (1) {
        int l = 2 * i + 1, r = 2 * i + 2, smallest = i;
        if (l < h->size && h->data[l]->vruntime < h->data[smallest]->vruntime)
            smallest = l;
        if (r < h->size && h->data[r]->vruntime < h->data[smallest]->vruntime)
            smallest = r;
        if (smallest == i) break;
        heap_swap(h, i, smallest);
        i = smallest;
    }
}

static void rq_push(run_queue_t *h, task_t *t) {
    if (h->size >= SCHED_MAX_TASKS) return;
    int i = h->size++;
    h->data[i] = t;
    t->heap_idx = i;
    heap_up(h, i);
}

static task_t *rq_pop(run_queue_t *h) {
    if (h->size == 0) return NULL;
    task_t *top = h->data[0];
    top->heap_idx = -1;
    h->size--;
    if (h->size > 0) {
        h->data[0] = h->data[h->size];
        h->data[0]->heap_idx = 0;
        heap_down(h, 0);
    }
    return top;
}

__attribute__((unused)) static void rq_remove(run_queue_t *h, task_t *t) {
    int i = t->heap_idx;
    if (i < 0 || i >= h->size) return;
    t->heap_idx = -1;
    h->size--;
    if (i < h->size) {
        h->data[i] = h->data[h->size];
        h->data[i]->heap_idx = i;
        heap_up(h, i);
        heap_down(h, i);
    }
}

static inline uint64_t rq_min_vruntime(run_queue_t *h) {
    return h->size > 0 ? h->data[0]->vruntime : 0;
}

/* -------------------------------------------------------------------------
 * Глобальное состояние планировщика
 * ------------------------------------------------------------------------- */
typedef struct {
    run_queue_t  rq;                /* очередь готовых задач */
    task_t      *current;           /* текущая задача */
    task_t      *idle;              /* idle-задача (всегда runnable) */

    /* Двусвязный список всех задач */
    task_t      *all_head;
    uint32_t     task_count;

    /* Счётчик tid */
    uint32_t     next_tid;

    /* min_vruntime: нижняя граница, чтобы новые задачи не получали
       преимущество и старые не получали слишком много "долга" */
    uint64_t     min_vruntime;

    /* Spinlock */
    spinlock_t lock;

    /* Инициализирован? */
    bool         ready;

    /* TSC при старте квантования */
    uint64_t     slice_start_tsc;
} scheduler_t;

static scheduler_t g_sched;

/* -------------------------------------------------------------------------
 * Вспомогательные функции
 * ------------------------------------------------------------------------- */
static void kmemset_s(void *dst, uint8_t val, uint64_t n) {
    uint8_t *p = (uint8_t *)dst;
    while (n--) *p++ = val;
}

static void kstrncpy(char *dst, const char *src, int n) {
    int i = 0;
    while (i < n - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

/* Обновить min_vruntime по текущей очереди и current */
static void update_min_vruntime(void) {
    uint64_t mv = g_sched.min_vruntime;
    if (g_sched.current && g_sched.current != g_sched.idle)
        mv = g_sched.current->vruntime;
    if (g_sched.rq.size > 0) {
        uint64_t rq_min = rq_min_vruntime(&g_sched.rq);
        if (mv == 0 || rq_min < mv) mv = rq_min;
    }
    if (mv > g_sched.min_vruntime)
        g_sched.min_vruntime = mv;
}

/* Выбрать следующую задачу (не удаляя из очереди) */
__attribute__((unused)) static task_t *pick_next(void) {
    if (g_sched.rq.size > 0)
        return g_sched.rq.data[0];
    return g_sched.idle;
}

/* -------------------------------------------------------------------------
 * Idle-задача
 * ------------------------------------------------------------------------- */
static void idle_fn(void *arg) {
    (void)arg;
    while (1)
        __asm__ volatile ("hlt");
}

/* -------------------------------------------------------------------------
 * Выделение и инициализация TCB
 * ------------------------------------------------------------------------- */
static task_t *alloc_task(void) {
    task_t *t = (task_t *)kmalloc(sizeof(task_t));
    if (!t) return NULL;
    kmemset_s(t, 0, sizeof(task_t));
    t->heap_idx = -1;
    return t;
}

/* -------------------------------------------------------------------------
 * Подготовка начального стека kernel-потока
 *
 * Стек при первом запуске должен выглядеть как после прерывания:
 *   [стек растёт вниз]
 *   ss        ← rsp+0x28  (фиктивный, kernel ss=0x10)
 *   rsp       ← rsp+0x20  (начальный rsp = вершина стека)
 *   rflags    ← rsp+0x18  (IF=1)
 *   cs        ← rsp+0x10  (kernel cs=0x08)
 *   rip       ← rsp+0x08  (адрес функции)
 *   error     ← rsp+0x00  (0)
 *   + 15 регистров (r15..rax) = 15*8 = 120 байт
 *
 * Итого: cpu_frame_t = 20 * 8 = 160 байт на стеке.
 * task->ctx.rsp = указатель на этот кадр.
 * ------------------------------------------------------------------------- */

/* cpu_frame_t layout (должен совпадать с InterruptControl.c): */
typedef struct __attribute__((packed)) {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rdx, rcx, rbx, rax;
    uint64_t error_code;
    uint64_t rip, cs, rflags, rsp, ss;
} sched_frame_t;

/* Обёртка: задача завершилась нормально → вызываем sched_exit */
static void task_wrapper(void (*fn)(void *), void *arg) {
    fn(arg);
    sched_exit();
}

static int setup_kstack(task_t *t, void (*fn)(void *), void *arg) {
    DBG_VAL("SC", "setup_kstack: request size", t->kstack_size);
    uint8_t *stack = (uint8_t *)kmalloc(t->kstack_size);
    DBG_VAL("SC", "setup_kstack: kmalloc result", (uint64_t)(uintptr_t)stack);
    if (!stack) {
        DBG_MSG("SC", "setup_kstack: FAILED - kmalloc returned NULL");
        return -1;
    }
    DBG_VAL("SC", "setup_kstack: zeroing stack", (uint64_t)(uintptr_t)stack);
    kmemset_s(stack, 0, t->kstack_size);
    t->kstack_base = stack;

    /* Вершина стека (выровнена на 16) */
    uint8_t *top = stack + t->kstack_size;
    top = (uint8_t *)((uint64_t)top & ~0xFULL);

    /* Резервируем место под sched_frame_t */
    top -= sizeof(sched_frame_t);
    sched_frame_t *frame = (sched_frame_t *)top;
    kmemset_s(frame, 0, sizeof(sched_frame_t));

    /* RIP указывает на task_wrapper */
    frame->rip    = (uint64_t)(uintptr_t)task_wrapper;
    frame->cs     = 0x08;                  /* kernel code segment */
    frame->rflags = 0x202;                 /* IF=1, reserved=1 */
    frame->rsp    = (uint64_t)(uintptr_t)(top + sizeof(sched_frame_t));
    frame->ss     = 0x10;                  /* kernel data segment */

    /* Аргументы task_wrapper через регистры (System V AMD64 ABI):
       rdi = fn, rsi = arg */
    frame->rdi = (uint64_t)(uintptr_t)fn;
    frame->rsi = (uint64_t)(uintptr_t)arg;

    t->ctx.rsp = (uint64_t)(uintptr_t)top;
    t->ctx.cr3 = 0; /* kernel PML4 — берётся из CR3 текущего контекста */
    t->kstack_top = (uint64_t)(uintptr_t)top;
    return 0;
}

/* -------------------------------------------------------------------------
 * sched_init
 * ------------------------------------------------------------------------- */
int sched_init(void) {
    dbg_puts("[SC] sched_init: START\n");
    
    kmemset_s(&g_sched, 0, sizeof(g_sched));
    g_sched.next_tid = 1;
    g_sched.lock     = 0;
    g_sched.ready    = false;

    dbg_puts("[SC] sched_init: calling kmalloc for idle\n");
    task_t *idle = (task_t *)kmalloc(sizeof(task_t));
    dbg_puts("[SC] sched_init: kmalloc result=");
    dbg_hex64((uint64_t)(uintptr_t)idle);
    dbg_puts("\n");
    
    if (!idle) return -1;

    idle->tid      = 0;
    idle->state    = TASK_RUNNABLE;
    idle->priority = SCHED_PRIO_MAX;
    idle->weight   = prio_to_w(SCHED_PRIO_MAX);
    idle->vruntime = 0;
    idle->kstack_size = SCHED_KSTACK_SIZE;
    kstrncpy(idle->name, "idle", sizeof(idle->name));

    if (setup_kstack(idle, idle_fn, NULL) != 0) {
        kfree(idle);
        return -1;
    }

    g_sched.idle     = idle;
    g_sched.current  = idle;
    g_sched.all_head = idle;
    g_sched.task_count = 1;

    interrupt_set_scheduler_tick(sched_tick);

    g_sched.ready = true;
    return 0;
}

/* -------------------------------------------------------------------------
 * sched_create_kthread
 * ------------------------------------------------------------------------- */
int sched_create_kthread(void (*fn)(void *), void *arg,
                         const char *name, int prio) {
    if (!fn) return -1;

    task_t *t = alloc_task();
    if (!t) return -1;

    spinlock_acquire(&g_sched.lock);

    t->tid       = g_sched.next_tid++;
    t->state     = TASK_RUNNABLE;
    t->priority  = prio;
    t->weight    = prio_to_w(prio);
    t->kstack_size = SCHED_KSTACK_SIZE;
    kstrncpy(t->name, name ? name : "kthread", sizeof(t->name));

    /* Новая задача получает min_vruntime чтобы не уйти в долг */
    update_min_vruntime();
    t->vruntime = g_sched.min_vruntime;

    if (setup_kstack(t, fn, arg) != 0) {
        spinlock_release(&g_sched.lock);
        kfree(t);
        return -1;
    }

    /* Добавляем в список всех задач */
    t->next_all = g_sched.all_head;
    if (g_sched.all_head) g_sched.all_head->prev_all = t;
    g_sched.all_head = t;
    g_sched.task_count++;

    /* Добавляем в очередь */
    rq_push(&g_sched.rq, t);

    spinlock_release(&g_sched.lock);

    DBG_VAL("SC", "created tid", t->tid);
    return (int)t->tid;
}

/* -------------------------------------------------------------------------
 * Ядро переключения контекста
 *
 * Вызывается из sched_tick (в контексте прерывания, на СТЕКЕ прерванной
 * задачи). frame_rsp — значение RSP на момент входа в обработчик, т.е.
 * указатель на cpu_frame_t (см. setup_kstack) на СОБСТВЕННОМ стеке
 * прерванной задачи.
 *
 * Что делаем:
 *   1. Запоминаем frame_rsp как ctx.rsp прерванной задачи "как есть" —
 *      это её собственный стек, его содержимое НЕ трогаем.
 *   2. Обновляем vruntime текущей задачи.
 *   3. Если текущая задача ещё RUNNING — переводим в RUNNABLE, кладём в rq.
 *   4. Выбираем следующую задачу.
 *   5. Возвращаем next->ctx.rsp — это RSP на СОБСТВЕННОМ стеке next,
 *      куда naked-стаб (irq0_timer_isr) переключит RSP перед pop+iretq.
 *
 * ВАЖНО: раньше здесь копировалось содержимое next_frame поверх frame
 * (т.е. поверх стека ПРЕРВАННОЙ задачи). Это ломало сохранённое состояние
 * prev — при следующем переключении на неё восстанавливались чужие
 * регистры, что и приводило к #GP. Правильный подход — не копировать
 * данные между стеками, а переключать сам регистр RSP на чужой стек.
 *
 * Мы не переключаем CR3 здесь, т.к. все kernel-потоки в одном адресном
 * пространстве. Для user-mode нужно добавить переключение CR3.
 * ------------------------------------------------------------------------- */
static uint64_t context_switch(uint64_t frame_rsp) {
    task_t *prev = g_sched.current;
    uint64_t now_tsc = rdtsc();

    /* Вычисляем delta реального времени */
    uint64_t delta_tsc = now_tsc - g_sched.slice_start_tsc;
    uint64_t delta_ns  = tsc_to_ns(delta_tsc);

    /* Обновляем vruntime: delta * (NICE_0_WEIGHT / weight) */
    if (prev && prev != g_sched.idle) {
        uint64_t weighted = (delta_ns * NICE_0_WEIGHT) / prev->weight;
        prev->vruntime     += weighted;
        prev->total_runtime += delta_ns;
    }

    update_min_vruntime();

    /* Сохраняем rsp текущей задачи — это её собственный стек, не трогаем */
    if (prev) {
        prev->ctx.rsp = frame_rsp;
        if (prev->state == TASK_RUNNING) {
            prev->state = TASK_RUNNABLE;
            if (prev != g_sched.idle)
                rq_push(&g_sched.rq, prev);
        }
        /* SLEEPING / ZOMBIE — не кладём в очередь */
    }

    /* Выбираем следующую задачу */
    task_t *next = rq_pop(&g_sched.rq);
    if (!next) next = g_sched.idle;

    next->state   = TASK_RUNNING;
    next->switches++;
    g_sched.current = next;
    g_sched.slice_start_tsc = now_tsc;

    /* Возвращаем RSP следующей задачи — на него переключится сам стек
     * (не данные) в naked-стабе irq0_timer_isr, после чего оттуда же
     * будут выполнены pop регистров и iretq. */
    return next->ctx.rsp;
}

/* -------------------------------------------------------------------------
 * sched_tick — обработчик таймерного прерывания
 *
 * frame_rsp — RSP на момент входа (см. irq0_timer_isr в InterruptControl.c).
 * Возвращает RSP, на который стаб должен переключиться перед pop+iretq:
 * либо тот же frame_rsp (без переключения), либо ctx.rsp следующей задачи.
 * ------------------------------------------------------------------------- */
uint64_t sched_tick(uint64_t frame_rsp) {
    if (!g_sched.ready) return frame_rsp;

    /* Курсор (PNG + альфа) и "тикающие" часы — рисуются вне блокировки
     * планировщика, чтобы не задерживать переключение контекста. */
    extern void ui_tick(void);
    ui_tick();

    spinlock_acquire(&g_sched.lock);

    task_t *cur = g_sched.current;

    /* Проверяем: исчерпан ли квант? */
    uint64_t delta_tsc = rdtsc() - g_sched.slice_start_tsc;
    uint64_t delta_ns  = tsc_to_ns(delta_tsc);

    bool need_switch = (delta_ns >= SCHED_TIMESLICE_NS);

    /* Также переключаемся если есть задача с меньшим vruntime */
    if (!need_switch && g_sched.rq.size > 0 && cur != g_sched.idle) {
        task_t *best = g_sched.rq.data[0];
        /* Если лучшая задача отстала более чем на один квант — вытесняем */
        uint64_t lag = (cur->vruntime > best->vruntime)
                     ? (cur->vruntime - best->vruntime) : 0;
        if (lag > SCHED_TIMESLICE_NS)
            need_switch = true;
    }

    /* Всегда переключаемся если текущая — idle, а в очереди есть задачи */
    if (cur == g_sched.idle && g_sched.rq.size > 0)
        need_switch = true;

    uint64_t resume_rsp = frame_rsp;
    if (need_switch)
        resume_rsp = context_switch(frame_rsp);

    spinlock_release(&g_sched.lock);
    return resume_rsp;
}

/* -------------------------------------------------------------------------
 * sched_yield — добровольная передача CPU
 * ------------------------------------------------------------------------- */
void sched_yield(void) {
    /* Принудительно обнуляем slice_start чтобы сразу сработал switch */
    spinlock_acquire(&g_sched.lock);
    g_sched.slice_start_tsc = 0;
    spinlock_release(&g_sched.lock);
    /* Прерывание таймера само переключит контекст при следующем тике */
    __asm__ volatile ("hlt");
}

/* -------------------------------------------------------------------------
 * sched_exit — завершение текущей задачи
 * ------------------------------------------------------------------------- */
void sched_exit(void) {
    spinlock_acquire(&g_sched.lock);

    task_t *t = g_sched.current;
    t->state = TASK_ZOMBIE;

    /* Убираем из списка всех задач */
    if (t->prev_all) t->prev_all->next_all = t->next_all;
    if (t->next_all) t->next_all->prev_all = t->prev_all;
    if (g_sched.all_head == t) g_sched.all_head = t->next_all;
    g_sched.task_count--;

    DBG_VAL("SC", "task exit tid", t->tid);

    /* Принудительно переключаемся на следующую задачу */
    g_sched.slice_start_tsc = 0;
    spinlock_release(&g_sched.lock);

    /* Ждём пока таймер нас переключит — мы уже ZOMBIE, в очередь не вернёмся */
    while (1) __asm__ volatile ("hlt");
}

/* -------------------------------------------------------------------------
 * sched_sleep_ns
 * ------------------------------------------------------------------------- */
void sched_sleep_ns(uint64_t ns) {
    spinlock_acquire(&g_sched.lock);
    task_t *t = g_sched.current;
    t->state = TASK_SLEEPING;
    /* TODO: добавить в список таймеров для автопробуждения */
    /* Пока просто добровольно уходим — простейшая реализация */
    (void)ns;
    g_sched.slice_start_tsc = 0;
    spinlock_release(&g_sched.lock);
    while (t->state == TASK_SLEEPING)
        __asm__ volatile ("hlt");
}

/* -------------------------------------------------------------------------
 * sched_wake
 * ------------------------------------------------------------------------- */
void sched_wake(uint32_t tid) {
    spinlock_acquire(&g_sched.lock);
    task_t *t = g_sched.all_head;
    while (t) {
        if (t->tid == tid && t->state == TASK_SLEEPING) {
            t->state = TASK_RUNNABLE;
            /* Получает min_vruntime чтобы не уйти в долг за время сна */
            update_min_vruntime();
            if (t->vruntime < g_sched.min_vruntime)
                t->vruntime = g_sched.min_vruntime;
            rq_push(&g_sched.rq, t);
            break;
        }
        t = t->next_all;
    }
    spinlock_release(&g_sched.lock);
}

/* -------------------------------------------------------------------------
 * sched_current / sched_find / sched_task_count
 * ------------------------------------------------------------------------- */
task_t *sched_current(void) { return g_sched.current; }

task_t *sched_find(uint32_t tid) {
    task_t *t = g_sched.all_head;
    while (t) {
        if (t->tid == tid) return t;
        t = t->next_all;
    }
    return NULL;
}

uint32_t sched_task_count(void) { return g_sched.task_count; }