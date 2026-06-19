/*
InterruptControl.c
Production-ready x86_64 Interrupt & Exception Manager
ИСПРАВЛЕНО: C23 bool, удален no_caller_saved_registers, исправлены опечатки
*/
#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "debug_out.h"

/* ==========================================================================
БАЗОВЫЕ ТИПЫ И МАКРОСЫ
========================================================================== */
typedef uint64_t           phys_addr_t;
typedef uint64_t           virt_addr_t;

/* C23 совместимость для bool */
#if !defined(__cplusplus) && (!defined(__STDC_VERSION__) || __STDC_VERSION__ < 202311L)
typedef _Bool bool;
#endif

#define true  1
#define false 0
#define NULL ((void*)0)
#define PAGE_SIZE       4096ULL
#define IRQ_BASE        0x20
#define TIMER_VECTOR    (IRQ_BASE + 0)
#define DIRECT_MAP_OFF  0xFFFF880000000000ULL

/* ==========================================================================
CPU FRAME & IDT СТРУКТУРЫ
========================================================================== */
typedef struct __attribute__((packed)) {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rdx, rcx, rbx, rax;
    uint64_t error_code;
    uint64_t rip, cs, rflags, rsp, ss;
} cpu_frame_t;

typedef struct __attribute__((packed)) { uint16_t limit; uint64_t base; } idtr_t;

typedef struct __attribute__((packed)) {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
} idt_entry_t;

#define IDT_ENTRIES 256
#define IDT_GATE_INT  0x8E
#define IDT_GATE_TRAP 0x8F

static idt_entry_t g_idt[IDT_ENTRIES] __attribute__((aligned(16)));
typedef void (*isr_handler_t)(uint64_t num, uint64_t err, cpu_frame_t *frame);
static isr_handler_t g_isr_handlers[IDT_ENTRIES];

/* ==========================================================================
АППАРАТНЫЕ ОПЕРАЦИИ
========================================================================== */
static inline uint8_t inb(uint16_t port) { 
    uint8_t v; 
    __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port)); 
    return v; 
}

static inline void outb(uint16_t port, uint8_t v) { 
    __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(port)); 
}

static inline void cli(void) { __asm__ volatile ("cli" ::: "memory"); }
static inline void sti(void) { __asm__ volatile ("sti" ::: "memory"); }
static inline void hlt(void) { __asm__ volatile ("hlt" ::: "memory"); }

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi; 
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline uint64_t read_cr2(void) {
    uint64_t v; 
    __asm__ volatile ("mov %%cr2, %0" : "=r"(v)); 
    return v;
}

static inline void lidt(const idtr_t *ptr) { 
    __asm__ volatile ("lidt %0" :: "m"(*ptr) : "memory"); 
}

/* ==========================================================================
IDT УПРАВЛЕНИЕ
========================================================================== */
static void idt_set_gate(uint8_t num, void *handler, uint8_t selector, uint8_t ist, uint8_t type) {
    uint64_t offset = (uint64_t)handler;
    idt_entry_t *e = &g_idt[num];
    e->offset_low   = (uint16_t)(offset & 0xFFFF);
    e->selector     = selector;
    e->ist          = ist & 0x7;
    e->type_attr    = type;
    e->offset_mid   = (uint16_t)((offset >> 16) & 0xFFFF);
    e->offset_high  = (uint32_t)((offset >> 32) & 0xFFFFFFFF);
    e->zero         = 0;
}

static void idt_load(void) {
    idtr_t idtr = { .limit = sizeof(g_idt) - 1, .base = (uint64_t)g_idt };
    lidt(&idtr);
}

/* ==========================================================================
VGA PANIC & PAGE FAULT HANDLER
========================================================================== */
static void vga_panic(const char *msg) {
    /*
     * ВАЖНО: после vmm_activate() физический адрес 0xB8000 не обязательно
     * замаплен 1:1 в текущем адресном пространстве. Прямая запись по этому
     * адресу может вызвать вложенный #PF -> #DF -> triple fault, из-за
     * чего сообщение об ошибке никогда не будет видно (система просто
     * "зависает"/перезагружается без вывода). Поэтому выводим панику ТОЛЬКО
     * через COM1 (порт ввода-вывода, не требует MMU).
     */
    dbg_puts("\n*** KERNEL PANIC ***\n");
    dbg_puts(msg);
    dbg_puts("\n*** halted ***\n");
    while (1) hlt();
}

static void format_hex(uint64_t val, char *buf) {
    const char hex[] = "0123456789ABCDEF";
    buf[0] = '0'; buf[1] = 'x'; buf[2] = '\0';
    int pos = 2;
    for (int i = 60; i >= 0; i -= 4) buf[pos++] = hex[(val >> i) & 0xF];
    buf[pos] = '\0';
}

__attribute__((interrupt))
void page_fault_handler(cpu_frame_t *frame, uint64_t err) {
    (void)frame;
    uint64_t cr2 = read_cr2();
    char addr_buf[20], err_buf[20];
    format_hex(cr2, addr_buf);
    format_hex(err, err_buf);
    const char *p = (err & 1) ? "P" : "NP";
    const char *wr = (err & 2) ? "W" : "R";
    const char *us = (err & 4) ? "U" : "S";

    static char panic_msg[256];
    int pos = 0;

    const char *prefix = "KERNEL PAGE FAULT\nADDR: 0x";
    for (int i = 0; prefix[i]; i++) panic_msg[pos++] = prefix[i];
    for (int i = 0; addr_buf[i]; i++) panic_msg[pos++] = addr_buf[i];

    const char *mid = "\nERR : 0x";
    for (int i = 0; mid[i]; i++) panic_msg[pos++] = mid[i];
    for (int i = 0; err_buf[i]; i++) panic_msg[pos++] = err_buf[i];

    const char *flags = "\nFLAGS: P= ";
    for (int i = 0; flags[i]; i++) panic_msg[pos++] = flags[i];
    panic_msg[pos++] = p[0]; panic_msg[pos++] = ' ';

    const char *wr_str = "W/R= ";
    for (int i = 0; wr_str[i]; i++) panic_msg[pos++] = wr_str[i];
    panic_msg[pos++] = wr[0]; panic_msg[pos++] = ' ';

    const char *us_str = "U/S= ";
    for (int i = 0; us_str[i]; i++) panic_msg[pos++] = us_str[i];
    panic_msg[pos++] = us[0]; panic_msg[pos++] = '\0';

    vga_panic(panic_msg);
}

/* ==========================================================================
DISPATCHERS & STUBS
========================================================================== */
static void exception_dispatch(uint8_t num, uint64_t err, cpu_frame_t *frame) {
    (void)frame;
    if (g_isr_handlers[num]) {
        g_isr_handlers[num](num, err, frame);
        return;
    }
    static const char *exc_names[32] = {
        "#DE Divide by Zero", "#DB Debug", "NMI", "#BP Breakpoint",
        "#OF Overflow", "#BR Bound Range", "#UD Invalid Opcode", "#NM Device Not Available",
        "#DF Double Fault", "#MF Coprocessor Segment", "#TS Invalid TSS", "#NP Segment Not Present",
        "#SS Stack Fault", "#GP General Protection", "#PF Page Fault", "#RF Reserved",
        "#MF x87 FPU", "#AC Alignment Check", "#MC Machine Check", "#XF SIMD FPE",
        "#VE Virtualization", "#RF Reserved", "#RF Reserved", "#RF Reserved",
        "#RF Reserved", "#RF Reserved", "#RF Reserved", "#RF Reserved",
        "#RF Reserved", "#RF Reserved", "#SX Security Exception", "#TF Triple Fault"
    };
    vga_panic((num < 32) ? exc_names[num] : "Unknown Exception");
}

static void irq_dispatch(uint8_t num, cpu_frame_t *frame) {
    (void)frame;
    if (g_isr_handlers[num]) g_isr_handlers[num](num, 0, frame);
}

/* --- Исключения БЕЗ кода ошибки --- */
__attribute__((interrupt)) void exc_0(cpu_frame_t *frame)  { exception_dispatch(0,  0, frame); }
__attribute__((interrupt)) void exc_1(cpu_frame_t *frame)  { exception_dispatch(1,  0, frame); }
__attribute__((interrupt)) void exc_2(cpu_frame_t *frame)  { exception_dispatch(2,  0, frame); }
__attribute__((interrupt)) void exc_3(cpu_frame_t *frame)  { exception_dispatch(3,  0, frame); }
__attribute__((interrupt)) void exc_4(cpu_frame_t *frame)  { exception_dispatch(4,  0, frame); }
__attribute__((interrupt)) void exc_5(cpu_frame_t *frame)  { exception_dispatch(5,  0, frame); }
__attribute__((interrupt)) void exc_6(cpu_frame_t *frame)  { exception_dispatch(6,  0, frame); }
__attribute__((interrupt)) void exc_7(cpu_frame_t *frame)  { exception_dispatch(7,  0, frame); }
__attribute__((interrupt)) void exc_9(cpu_frame_t *frame)  { exception_dispatch(9,  0, frame); }
__attribute__((interrupt)) void exc_15(cpu_frame_t *frame) { exception_dispatch(15, 0, frame); }
__attribute__((interrupt)) void exc_16(cpu_frame_t *frame) { exception_dispatch(16, 0, frame); }
__attribute__((interrupt)) void exc_18(cpu_frame_t *frame) { exception_dispatch(18, 0, frame); }
__attribute__((interrupt)) void exc_19(cpu_frame_t *frame) { exception_dispatch(19, 0, frame); }
__attribute__((interrupt)) void exc_20(cpu_frame_t *frame) { exception_dispatch(20, 0, frame); }

/* --- Исключения С кодом ошибки --- */
__attribute__((interrupt)) void exc_8(cpu_frame_t *frame, uint64_t err)  { exception_dispatch(8,  err, frame); }
__attribute__((interrupt)) void exc_10(cpu_frame_t *frame, uint64_t err) { exception_dispatch(10, err, frame); }
__attribute__((interrupt)) void exc_11(cpu_frame_t *frame, uint64_t err) { exception_dispatch(11, err, frame); }
__attribute__((interrupt)) void exc_12(cpu_frame_t *frame, uint64_t err) { exception_dispatch(12, err, frame); }
__attribute__((interrupt)) void exc_13(cpu_frame_t *frame, uint64_t err) { exception_dispatch(13, err, frame); }
__attribute__((interrupt)) void exc_17(cpu_frame_t *frame, uint64_t err) { exception_dispatch(17, err, frame); }
__attribute__((interrupt)) void exc_30(cpu_frame_t *frame, uint64_t err) { exception_dispatch(30, err, frame); }

/* --- IRQ Handlers --- */
__attribute__((interrupt)) void irq_0(cpu_frame_t *frame)  { irq_dispatch(0x20, frame); }
__attribute__((interrupt)) void irq_1(cpu_frame_t *frame)  { irq_dispatch(0x21, frame); }
__attribute__((interrupt)) void irq_2(cpu_frame_t *frame)  { irq_dispatch(0x22, frame); }
__attribute__((interrupt)) void irq_3(cpu_frame_t *frame)  { irq_dispatch(0x23, frame); }
__attribute__((interrupt)) void irq_4(cpu_frame_t *frame)  { irq_dispatch(0x24, frame); }
__attribute__((interrupt)) void irq_5(cpu_frame_t *frame)  { irq_dispatch(0x25, frame); }
__attribute__((interrupt)) void irq_6(cpu_frame_t *frame)  { irq_dispatch(0x26, frame); }
__attribute__((interrupt)) void irq_7(cpu_frame_t *frame)  { irq_dispatch(0x27, frame); }
__attribute__((interrupt)) void irq_8(cpu_frame_t *frame)  { irq_dispatch(0x28, frame); }
__attribute__((interrupt)) void irq_9(cpu_frame_t *frame)  { irq_dispatch(0x29, frame); }
__attribute__((interrupt)) void irq_10(cpu_frame_t *frame) { irq_dispatch(0x2A, frame); }
__attribute__((interrupt)) void irq_11(cpu_frame_t *frame) { irq_dispatch(0x2B, frame); }
__attribute__((interrupt)) void irq_12(cpu_frame_t *frame) { irq_dispatch(0x2C, frame); }
__attribute__((interrupt)) void irq_13(cpu_frame_t *frame) { irq_dispatch(0x2D, frame); }
__attribute__((interrupt)) void irq_14(cpu_frame_t *frame) { irq_dispatch(0x2E, frame); }
__attribute__((interrupt)) void irq_15(cpu_frame_t *frame) { irq_dispatch(0x2F, frame); }

/* ==========================================================================
PIC, APIC & IOAPIC
========================================================================== */
static void pic_remap(uint8_t off1, uint8_t off2) {
    uint8_t m1 = inb(0x21), m2 = inb(0xA1);
    outb(0x20, 0x11); outb(0xA0, 0x11);
    outb(0x21, off1); outb(0xA1, off2);
    outb(0x21, 0x04); outb(0xA1, 0x02);
    outb(0x21, 0x01); outb(0xA1, 0x01);
    outb(0x21, m1); outb(0xA1, m2);
}
static void pic_disable(void) { outb(0x21, 0xFF); outb(0xA1, 0xFF); }

#define APIC_BASE_MSR    0x1B
#define APIC_SVR         0xF0
#define APIC_EOI         0xB0
#define APIC_LVT_TIMER   0x320
#define APIC_LVT_LINT0   0x350
#define APIC_LVT_LINT1   0x360
#define APIC_LVT_ERR     0x370
#define APIC_INIT_CNT    0x380
#define APIC_CURR_CNT    0x390
#define APIC_DIV_CONF    0x3E0
#define IOAPIC_ID   0x00
#define IOAPIC_VER  0x01
#define IOAPIC_RED  0x10

static volatile uint32_t *g_lapic_base;
static volatile uint32_t *g_ioapic_base;

static inline void lapic_write(uint32_t reg, uint32_t val) { g_lapic_base[reg >> 2] = val; __asm__ volatile ("mfence" ::: "memory"); }
static inline uint32_t lapic_read(uint32_t reg) { uint32_t v = g_lapic_base[reg >> 2]; __asm__ volatile ("mfence" ::: "memory"); return v; }
static inline void ioapic_write(uint32_t reg, uint32_t val) { g_ioapic_base[0] = reg; __asm__ volatile ("mfence" ::: "memory"); g_ioapic_base[4] = val; __asm__ volatile ("mfence" ::: "memory"); }
static inline uint32_t ioapic_read(uint32_t reg) { g_ioapic_base[0] = reg; __asm__ volatile ("mfence" ::: "memory"); uint32_t v = g_ioapic_base[4]; __asm__ volatile ("mfence" ::: "memory"); return v; }

void apic_send_eoi(uint8_t vector) { (void)vector; lapic_write(APIC_EOI, 0); }

static void apic_init(void) {
    uint64_t msr = rdmsr(APIC_BASE_MSR);
    g_lapic_base = (volatile uint32_t *)(uintptr_t)((msr & 0xFFFFFFFFFFFFF000ULL) + DIRECT_MAP_OFF);
    lapic_write(APIC_SVR, 0x100 | 0xFF);
    lapic_write(APIC_LVT_LINT0, 0x10000 | 0xFF);
    lapic_write(APIC_LVT_LINT1, 0x10000 | 0xFF);
    lapic_write(APIC_LVT_ERR, 0x10000 | 0xFF);
}

static void ioapic_init(uint64_t phys_addr) {
    g_ioapic_base = (volatile uint32_t *)(uintptr_t)(phys_addr + DIRECT_MAP_OFF);
    uint32_t ver = ioapic_read(IOAPIC_VER);
    uint8_t entries = ((ver >> 16) & 0xFF) + 1;
    for (uint8_t i = 0; i < entries; i++) {
        ioapic_write(IOAPIC_RED + (2 * i), 0x00010000 | (IRQ_BASE + i));
        ioapic_write(IOAPIC_RED + (2 * i) + 1, 0x00000000);
    }
}

void ioapic_route_irq(uint8_t irq, uint8_t vector, uint32_t dest_lapic_id, bool level, bool low_pol) {
    uint32_t lo = vector | (1 << 8);
    if (level) lo |= (1 << 15);
    if (low_pol) lo |= (1 << 13);
    ioapic_write(IOAPIC_RED + (2 * irq), lo);
    ioapic_write(IOAPIC_RED + (2 * irq) + 1, dest_lapic_id << 24);
}

/* ==========================================================================
APIC TIMER CALIBRATION
========================================================================== */
static volatile bool g_pit_calib_done = false;
static uint32_t g_apic_bus_freq = 0;

__attribute__((interrupt))
void pit_calib_stub(cpu_frame_t *frame) {
    (void)frame;
    g_pit_calib_done = true;
    /* EOI для PIC (работаем через PIC во время калибровки) */
    outb(0x20, 0x20);  /* master PIC EOI */
    /* EOI для LAPIC на всякий случай */
    lapic_write(APIC_EOI, 0);
    /* Маскируем IRQ0 чтобы больше не прерывал */
    outb(0x21, inb(0x21) | 0x01);
}

static void apic_calibrate_timer(void) {
    uint16_t cs; __asm__ volatile ("mov %%cs, %0" : "=r"(cs));
    idt_set_gate(0x20, pit_calib_stub, cs, 0, IDT_GATE_INT);
    idt_load();

    /* Временно открываем LINT0 в режиме ExtINT чтобы PIC IRQ0 дошёл до CPU */
    lapic_write(APIC_LVT_LINT0, 0x00700);  /* ExtINT, не маскирован */

    /* PIT: ~10 мс (divisor=11931, частота 1193182 Гц) */
    uint16_t divisor = 11931;
    outb(0x43, 0x34);
    outb(0x40, (uint8_t)(divisor & 0xFF));
    outb(0x40, (uint8_t)((divisor >> 8) & 0xFF));

    /* Разрешаем только IRQ0 на master PIC */
    outb(0x21, 0xFE);
    outb(0xA1, 0xFF);

    lapic_write(APIC_DIV_CONF, 0x03);  /* делитель /16 */
    lapic_write(APIC_LVT_TIMER, 0x10000 | 0xFF);  /* маскируем таймер APIC */
    lapic_write(APIC_INIT_CNT, 0xFFFFFFFF);

    g_pit_calib_done = false;
    sti();

    /* Ждём одного тика PIT (~10 мс) */
    uint32_t timeout = 10000000;
    while (!g_pit_calib_done && timeout--) __asm__ volatile ("pause");

    cli();

    uint32_t elapsed = 0xFFFFFFFF - lapic_read(APIC_CURR_CNT);

    /* Закрываем LINT0 обратно */
    lapic_write(APIC_LVT_LINT0, 0x10000 | 0xFF);

    pic_disable();

    if (g_pit_calib_done && elapsed > 1000) {
        g_apic_bus_freq = elapsed * 100;  /* elapsed за 10мс -> частота в Гц */
    } else {
        /* Калибровка не удалась — используем типичное значение для QEMU */
        g_apic_bus_freq = 100000000;  /* 100 МГц */
    }
}

static void apic_timer_start(uint32_t freq_hz) {
    if (g_apic_bus_freq == 0) g_apic_bus_freq = 100000000;
    uint32_t div = 16;
    uint32_t init_cnt = (g_apic_bus_freq / div) / freq_hz;
    lapic_write(APIC_DIV_CONF, 0x03);
    lapic_write(APIC_INIT_CNT, init_cnt);
    lapic_write(APIC_LVT_TIMER, 0x20000 | TIMER_VECTOR);
}

/* ==========================================================================
РЕГИСТРАЦИЯ И ИНИЦИАЛИЗАЦИЯ
========================================================================== */
typedef void (*scheduler_tick_fn_t)(cpu_frame_t *frame);
static scheduler_tick_fn_t g_sched_tick = NULL;

int interrupt_register_handler(uint16_t vector, isr_handler_t handler) {
    if (vector >= IDT_ENTRIES) return -1;
    g_isr_handlers[vector] = handler;
    return 0;
}

void interrupt_set_scheduler_tick(scheduler_tick_fn_t fn) { g_sched_tick = fn; }
void interrupt_enable(void) { sti(); }
void interrupt_disable(void) { cli(); }

static void timer_handler(uint64_t num, uint64_t err, cpu_frame_t *frame) {
    (void)num; (void)err;
    if (g_sched_tick) g_sched_tick(frame);
}

int interrupt_init(uint64_t ioapic_phys_addr, uint32_t timer_freq_hz) {
    DBG_MSG("KR", "14a interrupt_init: pic");
    for (size_t i = 0; i < IDT_ENTRIES; i++) {
        g_isr_handlers[i] = NULL;
        __asm__ volatile ("" : "+m"(g_idt[i]) :: "memory");
    }
    pic_remap(0x20, 0x28);
    pic_disable();
    DBG_MSG("KR", "14b apic");
    apic_init();
    DBG_MSG("KR", "14c ioapic");
    ioapic_init(ioapic_phys_addr);
    DBG_MSG("KR", "14d timer calibrate");
    apic_calibrate_timer();

    g_isr_handlers[14] = NULL;
    g_isr_handlers[TIMER_VECTOR] = timer_handler;
 
    uint16_t cs; __asm__ volatile ("mov %%cs, %0" : "=r"(cs));

    /* Exceptions */
    idt_set_gate(0,  exc_0,  cs, 0, IDT_GATE_INT);
    idt_set_gate(1,  exc_1,  cs, 0, IDT_GATE_TRAP);
    idt_set_gate(2,  exc_2,  cs, 0, IDT_GATE_INT);
    idt_set_gate(3,  exc_3,  cs, 0, IDT_GATE_TRAP);
    idt_set_gate(4,  exc_4,  cs, 0, IDT_GATE_TRAP);
    idt_set_gate(5,  exc_5,  cs, 0, IDT_GATE_INT);
    idt_set_gate(6,  exc_6,  cs, 0, IDT_GATE_INT);
    idt_set_gate(7,  exc_7,  cs, 0, IDT_GATE_INT);
    idt_set_gate(8,  exc_8,  cs, 0, IDT_GATE_INT);
    idt_set_gate(9,  exc_9,  cs, 0, IDT_GATE_INT);
    idt_set_gate(10, exc_10, cs, 0, IDT_GATE_INT);
    idt_set_gate(11, exc_11, cs, 0, IDT_GATE_INT);
    idt_set_gate(12, exc_12, cs, 0, IDT_GATE_INT);
    idt_set_gate(13, exc_13, cs, 0, IDT_GATE_INT);
    idt_set_gate(14, page_fault_handler, cs, 0, IDT_GATE_INT);
    idt_set_gate(15, exc_15, cs, 0, IDT_GATE_INT);
    idt_set_gate(16, exc_16, cs, 0, IDT_GATE_INT);
    idt_set_gate(17, exc_17, cs, 0, IDT_GATE_INT);
    idt_set_gate(18, exc_18, cs, 0, IDT_GATE_INT);
    idt_set_gate(19, exc_19, cs, 0, IDT_GATE_INT);
    idt_set_gate(20, exc_20, cs, 0, IDT_GATE_INT);
    idt_set_gate(30, exc_30, cs, 0, IDT_GATE_INT);

    /* IRQs */
    idt_set_gate(0x20, irq_0,  cs, 0, IDT_GATE_INT);
    idt_set_gate(0x21, irq_1,  cs, 0, IDT_GATE_INT);
    idt_set_gate(0x22, irq_2,  cs, 0, IDT_GATE_INT);
    idt_set_gate(0x23, irq_3,  cs, 0, IDT_GATE_INT);
    idt_set_gate(0x24, irq_4,  cs, 0, IDT_GATE_INT);
    idt_set_gate(0x25, irq_5,  cs, 0, IDT_GATE_INT);
    idt_set_gate(0x26, irq_6,  cs, 0, IDT_GATE_INT);
    idt_set_gate(0x27, irq_7,  cs, 0, IDT_GATE_INT);
    idt_set_gate(0x28, irq_8,  cs, 0, IDT_GATE_INT);
    idt_set_gate(0x29, irq_9,  cs, 0, IDT_GATE_INT);
    idt_set_gate(0x2A, irq_10, cs, 0, IDT_GATE_INT);
    idt_set_gate(0x2B, irq_11, cs, 0, IDT_GATE_INT);
    idt_set_gate(0x2C, irq_12, cs, 0, IDT_GATE_INT);
    idt_set_gate(0x2D, irq_13, cs, 0, IDT_GATE_INT);
    idt_set_gate(0x2E, irq_14, cs, 0, IDT_GATE_INT);
    idt_set_gate(0x2F, irq_15, cs, 0, IDT_GATE_INT);

    DBG_MSG("KR", "14e idt_load");
    idt_load();
    apic_timer_start(timer_freq_hz ? timer_freq_hz : 100);
    DBG_MSG("KR", "14f sti");
    sti();
    return 0;
}

#ifdef __cplusplus
}
#endif