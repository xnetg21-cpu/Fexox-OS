#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* =========================================================================
 * Scheduler.h — CFS-подобный планировщик для FEXOS
 *
 * Алгоритм: у каждой задачи есть vruntime (виртуальное время выполнения).
 * Всегда запускается задача с минимальным vruntime. При вытеснении vruntime
 * увеличивается на реально потраченное время * (NICE_0_WEIGHT / weight).
 * Очередь — минимальная куча (min-heap) по vruntime, O(log n) вставка/удаление.
 * ========================================================================= */

/* Максимальное количество задач */
#define SCHED_MAX_TASKS     1024

/* Размер стека по умолчанию для kernel-потоков (16 KiB) */
#define SCHED_KSTACK_SIZE   (16 * 1024)

/* Квант времени в наносекундах (4 мс) */
#define SCHED_TIMESLICE_NS  4000000ULL

/* Вес для nice=0 (базовый) */
#define NICE_0_WEIGHT       1024

/* Приоритеты (nice-подобные, 0=нормальный, отрицательные=важнее) */
#define SCHED_PRIO_MIN   -20
#define SCHED_PRIO_MAX    19
#define SCHED_PRIO_NORMAL  0

/* -------------------------------------------------------------------------
 * Состояния задачи
 * ------------------------------------------------------------------------- */
typedef enum {
    TASK_RUNNING   = 0,  /* сейчас выполняется на CPU */
    TASK_RUNNABLE  = 1,  /* готова к выполнению, в очереди */
    TASK_SLEEPING  = 2,  /* ждёт события */
    TASK_ZOMBIE    = 3,  /* завершилась, ждёт сбора */
    TASK_DEAD      = 4,  /* собрана, память освобождена */
} task_state_t;

/* -------------------------------------------------------------------------
 * Сохранённый контекст CPU (kernel-side)
 * Сохраняем callee-saved регистры + rsp/rip для переключения.
 * При переключении через таймер полный контекст уже на стеке (cpu_frame_t),
 * здесь храним rsp чтобы вернуться в нужный стек.
 * ------------------------------------------------------------------------- */
typedef struct {
    uint64_t rsp;   /* указатель на сохранённый cpu_frame_t на стеке задачи */
    uint64_t cr3;   /* адрес PML4 задачи (для будущего user-mode) */
    /* callee-saved (rbx, rbp, r12-r15) сохраняются компилятором или вручную */
    uint64_t rbx;
    uint64_t rbp;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t rflags;
} task_context_t;

/* -------------------------------------------------------------------------
 * TCB — Task Control Block
 * ------------------------------------------------------------------------- */
typedef struct task {
    /* Идентификация */
    uint32_t        tid;            /* уникальный ID задачи */
    char            name[32];       /* имя для отладки */

    /* Состояние */
    task_state_t    state;

    /* CFS планирование */
    uint64_t        vruntime;       /* виртуальное время выполнения (нс) */
    uint64_t        start_time;     /* tsc при последнем запуске */
    int             priority;       /* nice-подобный приоритет [-20..19] */
    uint32_t        weight;         /* вычисляется из priority */

    /* Стек */
    void           *kstack_base;    /* начало аллоцированного стека */
    uint64_t        kstack_size;    /* размер стека */
    uint64_t        kstack_top;     /* rsp при первом запуске */

    /* Контекст */
    task_context_t  ctx;

    /* Адресное пространство (для будущего ring 3) */
    uint64_t        pml4_phys;      /* физический адрес PML4, 0 = kernel */

    /* Связный список всех задач (для итерации) */
    struct task    *next_all;
    struct task    *prev_all;

    /* Heap-индекс в очереди планировщика (-1 если не в очереди) */
    int             heap_idx;

    /* Статистика */
    uint64_t        total_runtime;  /* суммарное реальное время на CPU (нс) */
    uint64_t        switches;       /* количество переключений */
} task_t;

/* -------------------------------------------------------------------------
 * API планировщика
 * ------------------------------------------------------------------------- */

/* Инициализация (вызывать один раз после interrupt_init) */
int  sched_init(void);

/* Создать kernel-поток.
   fn     — функция потока (void fn(void *arg))
   arg    — аргумент
   name   — имя для отладки
   prio   — приоритет [SCHED_PRIO_MIN..SCHED_PRIO_MAX]
   Возвращает tid или -1 при ошибке. */
int  sched_create_kthread(void (*fn)(void *), void *arg,
                          const char *name, int prio);

/* Завершить текущую задачу (вызывается из тела потока) */
void sched_exit(void);

/* Добровольно отдать CPU */
void sched_yield(void);

/* Усыпить текущую задачу на ns наносекунд */
void sched_sleep_ns(uint64_t ns);

/* Разбудить задачу по tid */
void sched_wake(uint32_t tid);

/* Получить TCB текущей задачи */
task_t *sched_current(void);

/* Получить TCB по tid (NULL если не найдена) */
task_t *sched_find(uint32_t tid);

/* Обработчик таймерного прерывания — передаётся в interrupt_set_scheduler_tick */
void sched_tick(void *frame);

/* Статистика: число живых задач */
uint32_t sched_task_count(void);

#endif /* SCHEDULER_H */
