/*
 * main.c — точка входа ядра FEXOS
 */
#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "debug_out.h"
#include "Scheduler.h"
#include "VFS.h"
#include "ELF64.h"
#include "Syscall.h"
#include "Framebuffer.h"
#include "ui_extra.h"
#include "PS2Mouse.h"

/* -------------------------------------------------------------------------
 * IDE / AHCI драйверы
 * ------------------------------------------------------------------------- */
extern int       ide_init(void);
extern blkdev_t *ide_find_drive(const char *name);

extern int       ahci_init(void);
extern blkdev_t *ahci_find_drive(const char *name);

typedef struct __attribute__((packed)) {
    uint64_t magic;
    void    *mem_map;
    uint64_t mem_map_size;
    uint64_t desc_size;
    uint32_t desc_version;
    uint64_t kernel_entry;
    uint64_t pml4_phys;
    void    *gdt_ptr;
    void    *idt_ptr;
    fb_info_t fb;
    uint64_t rsdp_phys;   /* физический адрес ACPI RSDP, 0 если не найден */
} BOOT_INFO;

#define BOOT_INFO_MAGIC 0x4B45524E454C424FULL

extern int  mem_control_init(const void *efi_map, uint64_t map_size, uint64_t desc_size);
extern int  interrupt_init(uint64_t ioapic_phys_addr, uint32_t timer_freq_hz);
extern int  acpi_init(uint64_t rsdp_phys);
extern void *kmalloc(uint64_t size);
extern void  kfree(void *ptr);

/* -------------------------------------------------------------------------
 * VGA — до vmm_activate физический адрес, после виртуальный
 * ------------------------------------------------------------------------- */
#define VGA_PHYS  ((uint16_t *)0xB8000ULL)
#define VGA_VIRT  ((uint16_t *)(0xFFFF880000000000ULL + 0xB8000ULL))

static uint16_t *vga = VGA_PHYS;
static int vga_row;
static int vga_col;

static void kprint(const char *str) {
    dbg_puts(str);
    while (*str) {
        if (*str == '\n') {
            vga_row++;
            vga_col = 0;
        } else {
            vga[vga_row * 80 + vga_col] = (uint16_t)(0x0700 | (uint8_t)*str);
            vga_col++;
            if (vga_col >= 80) { vga_col = 0; vga_row++; }
        }
        if (vga_row >= 25) {
            for (int i = 0; i < 24 * 80; i++) vga[i] = vga[i + 80];
            for (int i = 24 * 80; i < 25 * 80; i++) vga[i] = 0x0720;
            vga_row = 24;
        }
        str++;
    }
}

static void kprint_err(const char *msg, int code) {
    kprint(msg);
    dbg_puts(" code=");
    if (code < 0) { dbg_putc('-'); dbg_dec64((uint64_t)(-(int64_t)code)); }
    else           { dbg_dec64((uint64_t)code); }
    dbg_puts("\n");
}

/*
 * kout — универсальный вывод: после fb_init и ДО отрисовки рабочего стола
 * пишет в fbcon (виден на экране как загрузочный лог), после
 * ui_draw_desktop() — только в serial (COM1), чтобы не рисовать текст
 * поверх уже готового рабочего стола (иначе строки вроде "[OK] scheduler"
 * остаются на экране навсегда, т.к. ui_tick()/ui_redraw_clock() трогают
 * только области курсора и часов, а не весь экран).
 */
static bool g_ui_desktop_shown = false;

static void kout(const char *str) {
    if (g_ui_desktop_shown) { dbg_puts(str); return; }
    if (fb_get_mode() == FB_MODE_LINEAR)
        fbcon_puts(str);
    else
        kprint(str);
}

/* =========================================================================
 * USER MODE — инфраструктура (TSS + SYSCALL/SYSRET + GDT)
 *
 * Сегменты в GDT которые нужны для ring 3 и SYSCALL:
 *   0x08 — kernel code  (CS, DPL=0)
 *   0x10 — kernel data  (SS, DPL=0)
 *   0x18 — user   code  (CS, DPL=3) — нужен для iret в ring 3
 *   0x20 — user   data  (SS, DPL=3)
 *   0x28 — TSS          (64-bit TSS descriptor, занимает 2 слота)
 *
 * TSS нужен для хранения RSP0 — стека ядра при входе из ring 3.
 * SYSCALL/SYSRET: MSR_STAR, MSR_LSTAR, MSR_SFMASK.
 * ========================================================================= */

/* TSS для x86-64 */
typedef struct __attribute__((packed)) {
    uint32_t reserved0;
    uint64_t rsp0;       /* стек ядра при входе из ring 3 */
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];     /* IST стеки для NMI/DF/etc */
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
} tss64_t;

/*
 * GDT layout (совместимый с SYSCALL/SYSRET AMD64):
 *   0x00 — null
 *   0x08 — kernel code64  (DPL=0)
 *   0x10 — kernel data    (DPL=0)
 *   0x18 — padding        (STAR[63:48] база = 0x18)
 *   0x20 — user data      (DPL=3)  SS при SYSRET = 0x18+8  = 0x20 ✓
 *   0x28 — user code64    (DPL=3)  CS при SYSRET = 0x18+16 = 0x28 ✓
 *   0x30 — TSS low        (64-bit TSS descriptor, 2 слота)
 *   0x38 — TSS high
 */
static uint64_t  g_gdt[8] __attribute__((aligned(16)));
static tss64_t   g_tss    __attribute__((aligned(16)));

/* Стек ядра для ring-3 syscall/interrupt входов */
#define SYSCALL_KSTACK_SIZE (16 * 1024)
static uint8_t g_syscall_kstack[SYSCALL_KSTACK_SIZE] __attribute__((aligned(16)));

static void gdt_set_entry(int idx, uint32_t base, uint32_t limit,
                          uint8_t access, uint8_t gran) {
    g_gdt[idx]  = (uint64_t)(limit & 0xFFFF);
    g_gdt[idx] |= (uint64_t)(base  & 0xFFFFFF) << 16;
    g_gdt[idx] |= (uint64_t)access             << 40;
    g_gdt[idx] |= (uint64_t)((limit >> 16) & 0x0F) << 48;
    g_gdt[idx] |= (uint64_t)(gran & 0xF0)      << 48;
    g_gdt[idx] |= (uint64_t)((base >> 24) & 0xFF) << 56;
}

static void gdt_set_tss(int idx, uint64_t base, uint32_t limit) {
    /* TSS descriptor занимает 16 байт (2 записи GDT) */
    g_gdt[idx]     = (uint64_t)(limit & 0xFFFF);
    g_gdt[idx]    |= (base & 0xFFFFFF) << 16;
    g_gdt[idx]    |= (uint64_t)0x89 << 40;         /* P=1, DPL=0, Type=TSS64 */
    g_gdt[idx]    |= (uint64_t)((limit >> 16) & 0x0F) << 48;
    g_gdt[idx]    |= (uint64_t)((base >> 24) & 0xFF) << 56;
    g_gdt[idx + 1] = (base >> 32);                 /* старшие 32 бита базы */
}

/* MSR номера */
#define MSR_EFER    0xC0000080
#define MSR_STAR    0xC0000081
#define MSR_LSTAR   0xC0000082
#define MSR_SFMASK  0xC0000084

static inline void wrmsr(uint32_t msr, uint64_t val) {
    __asm__ volatile ("wrmsr"
        :: "c"(msr), "a"((uint32_t)val), "d"((uint32_t)(val >> 32)));
}

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

/*
 * syscall_entry — точка входа SYSCALL инструкции.
 *
 * При входе (System V AMD64 syscall ABI):
 *   rax  = номер syscall
 *   rdi  = a1, rsi = a2, rdx = a3, r10 = a4, r8 = a5, r9 = a6
 *   rcx  = user rip (сохранено процессором)
 *   r11  = user rflags (сохранено процессором)
 *   rsp  = user rsp
 *
 * SYSV ABI для C-функций: аргументы rdi, rsi, rdx, rcx, r8, r9.
 * Но rcx занят user_rip — поэтому a4 передаётся через r10.
 * Перед call syscall_dispatch кладём r10 в rcx (4-й аргумент C).
 */
__attribute__((naked))
static void syscall_entry_stub(void) {
    __asm__ volatile (
        /* Сохраняем user rsp, переключаемся на kernel stack.
         * g_syscall_kstack_top хранит вершину ядерного стека. */
        "swapgs\n\t"
        "mov %%rsp, %%gs:0\n\t"            /* временно сохраняем user rsp через GS */

        /* Загружаем kernel stack */
        "mov %%gs:8, %%rsp\n\t"            /* kernel rsp из per-cpu (GS:8) */

        /* Строим мини-фрейм для возврата */
        "push %%r11\n\t"                   /* user rflags */
        "push %%rcx\n\t"                   /* user rip */
        "push %%gs:0\n\t"                  /* user rsp */

        /* Аргументы для syscall_dispatch:
         *   rdi=nr(rax), rsi=a1(rdi), rdx=a2(rsi), rcx=a3(rdx),
         *   r8=a4(r10),  r9=a5(r8)
         * Нам нужно переставить: rdi=rax, rsi=rdi, rdx=rsi, rcx=rdx, r8=r10, r9=r8 */
        "mov %%rax, %%rdi\n\t"             /* nr */
        /* rsi уже = a1 (rdi пользователя... нет, мы затёрли rdi) */
        /* Используем стек для сохранения оригинальных значений */
        "push %%rdi\n\t"                   /* сохраняем оригинальный rdi (= nr уже в rdi) */

        /* Перестройка аргументов без затирания:
         * на входе: rax=nr, rdi=a1, rsi=a2, rdx=a3, r10=a4, r8=a5
         * нужно:    rdi=nr, rsi=a1, rdx=a2, rcx=a3, r8=a4,  r9=a5 */
        "mov %%r8,  %%r9\n\t"   /* r9  = a5 */
        "mov %%r10, %%r8\n\t"   /* r8  = a4 */
        "mov %%rdx, %%rcx\n\t"  /* rcx = a3 */
        "mov %%rsi, %%rdx\n\t"  /* rdx = a2 */
        "pop %%rsi\n\t"         /* rsi = rdi_orig = a1 (pop того что пушнули) */
        /* rdi уже = nr (мы сделали mov %%rax, %%rdi выше) */

        "call syscall_dispatch\n\t"

        /* Восстанавливаем user context */
        "pop %%r10\n\t"                    /* user rsp (был на стеке) */
        "pop %%rcx\n\t"                    /* user rip */
        "pop %%r11\n\t"                    /* user rflags */
        "mov %%r10, %%rsp\n\t"             /* переключаемся на user stack */

        "swapgs\n\t"
        "sysretq\n\t"
        ::: "memory"
    );
}

/*
 * Инициализация per-cpu GS-данных для SYSCALL.
 * GS:0  = scratch (user rsp временно)
 * GS:8  = kernel rsp (вершина g_syscall_kstack)
 *
 * Это упрощённая однопроцессорная реализация через статическую структуру.
 */
typedef struct {
    uint64_t user_rsp_scratch;   /* GS:0  — временное место для user rsp */
    uint64_t kernel_rsp;         /* GS:8  — kernel stack top */
} percpu_t;

static percpu_t g_percpu __attribute__((aligned(16)));

#define MSR_GS_BASE      0xC0000101
#define MSR_KERNEL_GS    0xC0000102
#define CR4_FSGSBASE     (1U << 16)

static void usermode_init(void) {
    DBG_MSG("KR", "usermode_init: setup GDT+TSS+SYSCALL");

    /* Настраиваем TSS */
    uint64_t kstack_top = (uint64_t)(uintptr_t)
        (g_syscall_kstack + SYSCALL_KSTACK_SIZE);
    kstack_top &= ~0xFULL;  /* выравниваем на 16 */

    for (int i = 0; i < (int)sizeof(tss64_t); i++)
        ((uint8_t *)&g_tss)[i] = 0;
    g_tss.rsp0        = kstack_top;
    g_tss.iopb_offset = sizeof(tss64_t);

    /*
     * Строим GDT.
     * Порядок продиктован требованиями SYSRET (AMD64 Vol.2 §SYSRET):
     *   SYSRET загружает CS = STAR[63:48]+16, SS = STAR[63:48]+8.
     *   Мы кладём STAR[63:48]=0x18, тогда:
     *     SS = 0x18+8  = 0x20  → слот 4 (user data)  ✓
     *     CS = 0x18+16 = 0x28  → слот 5 (user code64) ✓
     */
    g_gdt[0] = 0;                                     /* 0x00 null        */
    gdt_set_entry(1, 0, 0xFFFFF, 0x9A, 0xA0);        /* 0x08 kernel code64 DPL=0 */
    gdt_set_entry(2, 0, 0xFFFFF, 0x92, 0xC0);        /* 0x10 kernel data   DPL=0 */
    g_gdt[3] = 0;                                     /* 0x18 padding (STAR base) */
    gdt_set_entry(4, 0, 0xFFFFF, 0xF2, 0xC0);        /* 0x20 user data     DPL=3 */
    gdt_set_entry(5, 0, 0xFFFFF, 0xFA, 0xA0);        /* 0x28 user code64   DPL=3 */
    gdt_set_tss  (6, (uint64_t)(uintptr_t)&g_tss,
                     sizeof(tss64_t) - 1);            /* 0x30 TSS (2 слота: 0x30, 0x38) */

    /* Загружаем GDT */
    struct __attribute__((packed)) { uint16_t limit; uint64_t base; } gdtr;
    gdtr.limit = sizeof(g_gdt) - 1;
    gdtr.base  = (uint64_t)(uintptr_t)g_gdt;
    __asm__ volatile ("lgdt %0" :: "m"(gdtr) : "memory");

    /*
     * КРИТИЧНО: после lgdt сегментный кэш CS всё ещё содержит старый
     * дескриптор от загрузчика. Нужен far-jump (lretq) чтобы перезагрузить CS.
     * Без этого последующий ltr может вызвать тройной fault.
     */
    __asm__ volatile (
        "pushq $0x08\n\t"            /* новый kernel CS */
        "lea 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        ::: "rax", "memory"
    );

    /* Перезагружаем регистры данных под новый GDT */
    __asm__ volatile (
        "mov $0x10, %%ax\n\t"   /* kernel data = 0x10 */
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%ss\n\t"
        "xor %%ax, %%ax\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        ::: "ax", "memory"
    );

    /* Загружаем TR — теперь безопасно: CS/SS корректны */
    __asm__ volatile ("ltr %0" :: "r"((uint16_t)0x30));

    /* Включаем FSGSBASE (CR4[16]): wrfsbase/rdfsbase в ring3
     * нужны для sys_arch_prctl (TLS/pthread).
     *
     * ВНИМАНИЕ: на некоторых CPU/QEMU -cpu моделях (например "qemu64")
     * бит CPUID.7.EBX[0] (FSGSBASE) не выставлен. Попытка установить
     * CR4.FSGSBASE при отсутствии поддержки вызывает #GP. Если этот
     * #GP не обработан корректно (или обработчик падает на запись
     * в немаппленный 0xB8000), система "зависает" без вывода —
     * именно это и происходило здесь. Проверяем CPUID перед записью CR4. */
    {
        uint32_t eax, ebx, ecx, edx;
        __asm__ volatile ("cpuid"
            : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
            : "a"(7), "c"(0));
        bool has_fsgsbase = (ebx & 1) != 0;

        uint64_t cr4;
        __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
        if (has_fsgsbase) {
            __asm__ volatile ("mov %0, %%cr4" :: "r"(cr4 | CR4_FSGSBASE) : "memory");
        } else {
            DBG_MSG("KR", "usermode_init: FSGSBASE not supported, skipping CR4 bit");
        }
    }

    /* Инициализируем percpu для SYSCALL: GS:0=scratch, GS:8=kernel_rsp */
    g_percpu.user_rsp_scratch = 0;
    g_percpu.kernel_rsp       = kstack_top;

    /*
     * MSR_GS_BASE     = &g_percpu  (активный kernel GS — читается как GS:x)
     * MSR_KERNEL_GS   = 0          (user GS; swapgs меняет их местами)
     * Если оба указывают на &g_percpu — после swapgs в syscall_entry
     * GS_BASE станет 0, и mov %%gs:0 даст #GP. Ставим user-сторону в 0.
     */
    wrmsr(MSR_GS_BASE,   (uint64_t)(uintptr_t)&g_percpu);
    wrmsr(MSR_KERNEL_GS, 0ULL);

    /* Включаем SCE (System Call Extensions) в EFER */
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | 1ULL);  /* бит 0 = SCE */

    /*
     * MSR_STAR:
     *   биты [47:32] = kernel CS при SYSCALL  → 0x08
     *   биты [63:48] = база для SYSRET        → 0x18
     *     SYSRET: CS = 0x18+16 = 0x28 (user code64) ✓
     *             SS = 0x18+8  = 0x20 (user data)   ✓
     */
    uint64_t star = ((uint64_t)0x0008 << 32) |
                    ((uint64_t)0x0018 << 48);
    wrmsr(MSR_STAR,   star);
    wrmsr(MSR_LSTAR,  (uint64_t)(uintptr_t)syscall_entry_stub);
    wrmsr(MSR_SFMASK, (1ULL << 9));  /* маскируем IF при входе в syscall */

    /* Инициализация подсистемы syscall (fd-таблица, shm, pipe) */
    syscall_init();

    DBG_MSG("KR", "usermode_init: GDT/TSS/SYSCALL ok");
}

/* =========================================================================
 * Тестовые kernel-потоки
 * ========================================================================= */
static void worker_a(void *arg) {
    (void)arg;
    uint32_t n = 0;
    while (1) {
        if ((n & 0xFFFFF) == 0)
            DBG_MSG("SC", "worker_a tick");
        n++;
        if (n == 0x300000) sched_exit();
    }
}

static void worker_b(void *arg) {
    (void)arg;
    uint32_t n = 0;
    while (1) {
        if ((n & 0xFFFFF) == 0)
            DBG_MSG("SC", "worker_b tick");
        n++;
        if (n == 0x300000) sched_exit();
    }
}

/* =========================================================================
 * ELF64 — попытка запустить /init (или другой статический ELF64 бинарник)
 *
 * Вызывается после монтирования корневой ФС.
 * Если файл не существует — просто пишем предупреждение и продолжаем.
 * ========================================================================= */
static void try_spawn_init(void) {
    /* Список кандидатов в порядке приоритета */
    static const char *init_paths[] = { "/init", "/bin/init", "/sbin/init", NULL };

    for (int i = 0; init_paths[i] != NULL; i++) {
        DBG_MSG("ELF", "trying to spawn ELF...");

        /* Проверяем существование файла */
        vfs_stat_t st;
        int sr = vfs_stat(init_paths[i], &st);
        if (sr != VFS_OK) continue;
        if (st.type != VFS_TYPE_FILE) continue;

        kprint("[ELF] spawning ");
        kprint(init_paths[i]);
        kprint("\n");

        int tid = elf64_spawn(init_paths[i], "init", 0);
        if (tid >= 0) {
            kprint("[OK] ELF64 process spawned (tid=");
            dbg_dec64((uint64_t)(uint32_t)tid);
            kprint(")\n");
            return;
        }

        /* Выводим код ошибки */
        kprint("[WARN] elf64_spawn failed, code=");
        if (tid < 0) { dbg_putc('-'); dbg_dec64((uint64_t)(uint32_t)(-tid)); }
        kprint("\n");
        return;  /* попробовали первый существующий файл — хватит */
    }

    kprint("[INFO] no /init found — skipping ELF spawn\n");
    DBG_MSG("ELF", "no init binary on fs");
}

/* =========================================================================
 * kernel_entry
 * ========================================================================= */
void kernel_entry(BOOT_INFO *info) {
    __asm__ volatile ("cli" ::: "memory");

    DBG_MSG("KR", "10 kernel_entry");
    vga_row = 0; vga_col = 0;
    vga = VGA_PHYS;

    if (!info)                          DBG_PANIC("KR", "info NULL");
    if (info->magic != BOOT_INFO_MAGIC) DBG_PANIC("KR", "bad magic");

    kprint("=== FEXOS Kernel ===\n");
    DBG_MSG("KR", "11 BOOT_INFO ok");

    /* --- Память --- */
    DBG_MSG("KR", "12 mem_control_init...");
    int ret = mem_control_init(info->mem_map, info->mem_map_size, info->desc_size);
    if (ret != 0) {
        kprint_err("[FATAL] mem_control_init", ret);
        DBG_PANIC("KR", "mem_control_init");
    }
    vga = VGA_VIRT;  /* vmm_activate уже случился внутри */
    kprint("[OK] memory\n");
    DBG_MSG("KR", "13 memory ok");

    /* --- ACPI (для настоящего shutdown/reboot на реальном железе) ---
     * Нужен активный DIRECT_MAP (уже есть после mem_control_init).
     * Не фатально при ошибке — Menu Shutdown/Reboot тогда просто
     * останутся на старых фолбэках (QEMU debug-порт / 8042 / triple fault). */
    DBG_MSG("KR", "13a acpi_init...");
    if (acpi_init(info->rsdp_phys) == 0) {
        kprint("[OK] acpi\n");
    } else {
        kprint("[INFO] no ACPI (rsdp not found / parse failed)\n");
    }

    /* --- Framebuffer --- */
    DBG_MSG("KR", "13b fb_init...");
    int fb_mode = fb_init(&info->fb);
    if (fb_mode == FB_MODE_LINEAR) {
        /* Double-buffer включаем ПОСЛЕ ui_draw_desktop, когда рабочий стол
         * уже нарисован. До этого пишем прямо во front-буфер.
         *
         * Причина: kmalloc(4 МБ) сразу после mem_control_init подвешивает
         * систему — аллокатор ещё не готов к таким запросам.
         * Отладочный вывод на этапе boot идёт через DBG_MSG/COM1. */
        fbcon_init(0, 0, fb_width(), fb_height(), FB_WHITE, FB_DARK_BLUE);
        fbcon_puts("=== FEXOS Kernel ===\n");
        DBG_MSG("KR", "13b fb linear OK");
    } else {
        /* GOP не нашли — продолжаем на VGA text через kprint */
        kprint("[INFO] no GOP, VGA text mode\n");
        DBG_MSG("KR", "13b fb VGA fallback");
    }

    /* --- Прерывания --- */
    DBG_MSG("KR", "14 interrupt_init...");
    ret = interrupt_init(0xFEC00000ULL, 100);
    if (ret != 0) {
        kprint_err("[FATAL] interrupt_init", ret);
        DBG_PANIC("KR", "interrupt_init");
    }
    kout("[OK] interrupts\n");
    DBG_MSG("KR", "15 interrupts ok");

    /* --- User mode инфраструктура --- */
    DBG_MSG("KR", "16 usermode_init...");
    usermode_init();
    kout("[OK] usermode\n");
    DBG_MSG("KR", "17 usermode ok");

    /*
     * VFS + блочный слой ДО планировщика.
     *
     * Порядок пробы устройств: virtio-blk → AHCI → IDE.
     * Монтируем первое найденное устройство как "/".
     *
     * Важно: IF=1 (sti) нужен для virtio-blk polling (QEMU BH).
     * IDE и AHCI работают на polling без прерываний, но sti не мешает.
     * AHCI требует активного VMM (DIRECT_MAP_OFFSET) — он уже включён
     * внутри mem_control_init, поэтому AHCI можно вызывать здесь.
     */
    __asm__ volatile ("sti" ::: "memory");

    DBG_MSG("KR", "20 vfs_init...");
    ret = vfs_init();
    if (ret != 0) {
        kprint_err("[WARN] vfs_init", ret);
        goto skip_fs;
    }
    kout("[OK] vfs\n");

    /* Регистрируем FAT32 один раз — до любых mount */
    fat32_register();

    /* ------------------------------------------------------------------
     * Блок 1: virtio-blk (QEMU, vda)
     * ------------------------------------------------------------------ */
    DBG_MSG("KR", "21 virtio_blk_init...");
    ret = virtio_blk_init();
    if (ret == 0) {
        kout("[OK] virtio-blk (vda)\n");
    } else {
        kprint_err("[INFO] virtio_blk_init (no device)", ret);
    }

    /* ------------------------------------------------------------------
     * Блок 2: AHCI/SATA (реальное железо или ich9-ahci в QEMU)
     * Регистрирует sda, sdb, … через blkdev_register.
     * ------------------------------------------------------------------ */
    DBG_MSG("KR", "22 ahci_init...");
    ret = ahci_init();
    if (ret == 0) {
        kout("[OK] AHCI\n");
    } else {
        kprint_err("[INFO] ahci_init (no AHCI HBA)", ret);
    }

    /* ------------------------------------------------------------------
     * Блок 3: IDE/ATA (legacy PIO + Bus Master DMA)
     * Регистрирует hda, hdb, hdc, hdd через blkdev_register.
     * Работает через I/O порты 0x1F0 / 0x170 — не нужен MMIO.
     * ------------------------------------------------------------------ */
    DBG_MSG("KR", "23 ide_init...");
    ret = ide_init();
    if (ret == 0) {
        kout("[OK] IDE\n");
    } else {
        kprint_err("[INFO] ide_init (no IDE drives)", ret);
    }

    /* ------------------------------------------------------------------
     * Монтируем первое доступное устройство как "/"
     * Приоритет: vda (virtio) > sda (AHCI) > hda (IDE)
     * ------------------------------------------------------------------ */
    {
        static const char *root_devs[] = { "vda", "sda", "hda", NULL };
        int mounted = 0;
        for (int di = 0; root_devs[di] != NULL; di++) {
            if (!blkdev_find(root_devs[di])) continue;

            DBG_MSG("KR", "24 vfs_mount /...");
            ret = vfs_mount(root_devs[di], "fat32", "/", 0);
            if (ret == 0) {
                kout("[OK] / mounted (FAT32, dev=");
                kout(root_devs[di]);
                kout(")\n");
                mounted = 1;
                break;
            }
            kprint_err("[WARN] vfs_mount failed for dev", ret);
        }

        if (mounted) {
            /* Тест: создать файл */
            int fd = vfs_open("/hello.txt", O_RDWR | O_CREAT);
            if (fd >= 0) {
                const char *msg = "Hello from FEXOS!\n";
                vfs_write(fd, msg, 18);
                vfs_seek(fd, 0, 0);
                char rbuf[64];
                int64_t n = vfs_read(fd, rbuf, 63);
                if (n > 0) { rbuf[n] = 0; kout(rbuf); }
                vfs_close(fd);
                kout("[OK] VFS r/w test\n");
            }
            /* readdir / */
            int dfd = vfs_open("/", O_DIRECTORY);
            if (dfd >= 0) {
                vfs_dirent_t de;
                kout("[LS /]\n");
                for (uint32_t i = 0; vfs_readdir(dfd, i, &de) == VFS_OK; i++) {
                    kout("  "); kout(de.name);
                    kout(de.type == VFS_TYPE_DIR ? "/\n" : "\n");
                }
                vfs_close(dfd);
            }

            /* --- ELF64: попытка запустить /init --- */
            try_spawn_init();

        } else {
            kout("[WARN] no root filesystem mounted\n");
            DBG_MSG("KR", "no root fs — continuing without storage");
        }
    }

skip_fs:;

    /* --- Double-buffer: включаем здесь, когда kmalloc полностью готов.
     * ui_draw_desktop нарисует рабочий стол в back-буфер и сделает fb_flip(). */
    if (fb_get_mode() == FB_MODE_LINEAR) {
        if (fb_enable_double_buffer() == 0)
            DBG_MSG("KR", "double-buffer enabled OK");
        else
            DBG_MSG("KR", "double-buffer failed, writing to front directly");
    }

    /* --- Рабочий стол UI --- */
    DBG_MSG("KR", "25 ui_draw_desktop...");
    ui_draw_desktop();
    DBG_MSG("KR", "25 ui ok");
    g_ui_desktop_shown = true;  /* с этого момента kout() -> serial only */

    DBG_MSG("KR", "25b ui_extra_init (cursor.png + tick clock)...");
    ui_extra_init();
    DBG_MSG("KR", "25b ui_extra ok");

    DBG_MSG("KR", "25c ps2_mouse_init...");
    if (ps2_mouse_init() != 0)
        DBG_MSG("KR", "25c ps2 mouse not found (курсор останется неподвижным)");
    else
        DBG_MSG("KR", "25c ps2 mouse ok");

    /* --- Планировщик (после mount — blk I/O уже не нужен на boot path) --- */
    DBG_MSG("KR", "18 sched_init...");
    ret = sched_init();
    if (ret != 0) {
        kprint_err("[FATAL] sched_init", ret);
        DBG_PANIC("KR", "sched_init");
    }
    kout("[OK] scheduler\n");
    DBG_MSG("KR", "19 sched ok");

    sched_create_kthread(worker_a, NULL, "worker_a",  0);
    sched_create_kthread(worker_b, NULL, "worker_b", -5);
    kout("[OK] kthreads\n");

    /* --- Запускаем --- */
    __asm__ volatile ("sti" ::: "memory");
    kout(">>> FEXOS running <<<\n");
    DBG_MSG("KR", "99 idle loop");

    /* Idle loop — планировщик сам переключит на потоки */
    while (1)
        __asm__ volatile ("hlt");
}

#ifdef __cplusplus
}
#endif