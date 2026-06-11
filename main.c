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
} BOOT_INFO;

#define BOOT_INFO_MAGIC 0x4B45524E454C424FULL

extern int  mem_control_init(const void *efi_map, uint64_t map_size, uint64_t desc_size);
extern int  interrupt_init(uint64_t ioapic_phys_addr, uint32_t timer_freq_hz);
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

/* GDT: null, kcode, kdata, ucode, udata, tss_low, tss_high */
static uint64_t  g_gdt[7] __attribute__((aligned(16)));
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

/* Заглушка обработчика системных вызовов (точка входа SYSCALL) */
__attribute__((naked))
static void syscall_entry_stub(void) {
    /* При входе: rip пользователя в rcx, rflags в r11, номер syscall в rax */
    __asm__ volatile (
        /* Сохраняем пользовательский rsp, переключаемся на kernel stack */
        "swapgs\n\t"                        /* будущее: GS.base = per-cpu данные */
        "mov %%rsp, %%r10\n\t"              /* r10 = user rsp */
        /* Здесь в реальной ОС: загружаем kernel rsp из per-cpu */
        /* Пока просто вызываем обработчик */
        "push %%r10\n\t"                    /* сохраняем user rsp */
        "push %%rcx\n\t"                    /* сохраняем user rip */
        "push %%r11\n\t"                    /* сохраняем user rflags */
        "call syscall_dispatch\n\t"
        "pop %%r11\n\t"
        "pop %%rcx\n\t"
        "pop %%rsp\n\t"
        "swapgs\n\t"
        "sysretq\n\t"
        ::: "memory"
    );
}

/* Диспетчер системных вызовов */
void syscall_dispatch(uint64_t nr, uint64_t a1, uint64_t a2,
                      uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    DBG_VAL("SC", "syscall nr", nr);
    /* Будущие syscall: 0=exit, 1=write, 2=read, ... */
}

static void usermode_init(void) {
    DBG_MSG("KR", "usermode_init: setup GDT+TSS+SYSCALL");

    /* Настраиваем TSS */
    uint64_t kstack_top = (uint64_t)(uintptr_t)
        (g_syscall_kstack + SYSCALL_KSTACK_SIZE);
    kstack_top &= ~0xFULL;  /* выравниваем на 16 */

    for (int i = 0; i < (int)sizeof(tss64_t); i++)
        ((uint8_t *)&g_tss)[i] = 0;
    g_tss.rsp0       = kstack_top;
    g_tss.iopb_offset = sizeof(tss64_t);

    /* Строим GDT */
    g_gdt[0] = 0;                                    /* null */
    gdt_set_entry(1, 0, 0xFFFFF, 0x9A, 0xA0);       /* 0x08 kernel code64 */
    gdt_set_entry(2, 0, 0xFFFFF, 0x92, 0xC0);       /* 0x10 kernel data */
    gdt_set_entry(3, 0, 0xFFFFF, 0xFA, 0xA0);       /* 0x18 user   code64 DPL=3 */
    gdt_set_entry(4, 0, 0xFFFFF, 0xF2, 0xC0);       /* 0x20 user   data   DPL=3 */
    gdt_set_tss  (5, (uint64_t)(uintptr_t)&g_tss,
                     sizeof(tss64_t) - 1);           /* 0x28 TSS */

    /* Загружаем GDT */
    struct __attribute__((packed)) { uint16_t limit; uint64_t base; } gdtr;
    gdtr.limit = sizeof(g_gdt) - 1;
    gdtr.base  = (uint64_t)(uintptr_t)g_gdt;
    __asm__ volatile ("lgdt %0" :: "m"(gdtr));

    /* Загружаем TR */
    __asm__ volatile ("ltr %0" :: "r"((uint16_t)0x28));

    /* Включаем SCE (System Call Extensions) в EFER */
    uint64_t efer = rdmsr(MSR_EFER);
    wrmsr(MSR_EFER, efer | 1);  /* бит 0 = SCE */

    /* MSR_STAR: биты [47:32] = kernel CS (0x08), биты [63:48] = user CS-16 (0x18-16=0x08 → 0x18|3)
       SYSCALL: CS = STAR[47:32],     SS = STAR[47:32]+8
       SYSRET:  CS = STAR[63:48]+16, SS = STAR[63:48]+8  */
    uint64_t star = ((uint64_t)0x0008 << 32) |  /* kernel CS = 0x08 */
                    ((uint64_t)0x0013 << 48);    /* user CS для sysret = 0x1B (0x18|3) − 16 = 0x13 */
    wrmsr(MSR_STAR,   star);
    wrmsr(MSR_LSTAR,  (uint64_t)(uintptr_t)syscall_entry_stub);
    wrmsr(MSR_SFMASK, (1 << 9));  /* маскируем IF при входе в syscall */

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

    /* --- Прерывания --- */
    DBG_MSG("KR", "14 interrupt_init...");
    ret = interrupt_init(0xFEC00000ULL, 100);
    if (ret != 0) {
        kprint_err("[FATAL] interrupt_init", ret);
        DBG_PANIC("KR", "interrupt_init");
    }
    kprint("[OK] interrupts\n");
    DBG_MSG("KR", "15 interrupts ok");

    /* --- User mode инфраструктура --- */
    DBG_MSG("KR", "16 usermode_init...");
    usermode_init();
    kprint("[OK] usermode\n");
    DBG_MSG("KR", "17 usermode ok");

    /*
     * VFS + virtio ДО планировщика: legacy virtio-blk в QEMU обрабатывает
     * queue notify асинхронно (BH). Нужны IF=1 и отсутствие вытеснения,
     * иначе poll зависает (used->idx не обновляется).
     */
    __asm__ volatile ("sti" ::: "memory");
    DBG_MSG("KR", "20 vfs_init...");
    ret = vfs_init();
    if (ret != 0) {
        kprint_err("[WARN] vfs_init", ret);
    } else {
        kprint("[OK] vfs\n");
        DBG_MSG("KR", "21 virtio_blk_init...");
        ret = virtio_blk_init();
        if (ret != 0) {
            kprint_err("[WARN] virtio_blk_init", ret);
        } else {
            kprint("[OK] virtio-blk (vda)\n");
            fat32_register();
            ret = vfs_mount("vda", "fat32", "/", 0);
            if (ret != 0) {
                kprint_err("[WARN] vfs_mount /", ret);
            } else {
                kprint("[OK] / mounted (FAT32)\n");
                /* Тест: создать файл */
                int fd = vfs_open("/hello.txt", O_RDWR | O_CREAT);
                if (fd >= 0) {
                    const char *msg = "Hello from FEXOS!\n";
                    vfs_write(fd, msg, 18);
                    vfs_seek(fd, 0, 0);
                    char rbuf[64];
                    int64_t n = vfs_read(fd, rbuf, 63);
                    if (n > 0) { rbuf[n] = 0; kprint(rbuf); }
                    vfs_close(fd);
                    kprint("[OK] VFS r/w test\n");
                }
                /* readdir / */
                int dfd = vfs_open("/", O_DIRECTORY);
                if (dfd >= 0) {
                    vfs_dirent_t de;
                    kprint("[LS /]\n");
                    for (uint32_t i = 0; vfs_readdir(dfd, i, &de) == VFS_OK; i++) {
                        kprint("  "); kprint(de.name);
                        kprint(de.type == VFS_TYPE_DIR ? "/\n" : "\n");
                    }
                    vfs_close(dfd);
                }
            }
        }
    }

    /* --- Планировщик (после mount — blk I/O уже не нужен на boot path) --- */
    DBG_MSG("KR", "18 sched_init...");
    ret = sched_init();
    if (ret != 0) {
        kprint_err("[FATAL] sched_init", ret);
        DBG_PANIC("KR", "sched_init");
    }
    kprint("[OK] scheduler\n");
    DBG_MSG("KR", "19 sched ok");

    sched_create_kthread(worker_a, NULL, "worker_a",  0);
    sched_create_kthread(worker_b, NULL, "worker_b", -5);
    kprint("[OK] kthreads\n");

    /* --- Запускаем --- */
    __asm__ volatile ("sti" ::: "memory");
    kprint(">>> FEXOS running <<<\n");
    DBG_MSG("KR", "99 idle loop");

    /* Idle loop — планировщик сам переключит на потоки */
    while (1)
        __asm__ volatile ("hlt");
}

#ifdef __cplusplus
}
#endif