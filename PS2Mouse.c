/*
 * PS2Mouse.c — драйвер PS/2-мыши (i8042, IRQ12) для FEXOS.
 *
 * Протокол (стандартный 3-байтный пакет PS/2-мыши):
 *   byte0: bit0=LMB bit1=RMB bit2=MMB bit3=1(always) bit4=X sign
 *          bit5=Y sign bit6=X overflow bit7=Y overflow
 *   byte1: dX (0..255, знак — из byte0 bit4)
 *   byte2: dY (0..255, знак — из byte0 bit5; ось Y у PS/2 растёт вверх,
 *          у экрана — вниз, поэтому знак инвертируется)
 *
 * IRQ12 срабатывает на КАЖДЫЙ байт — пакет из 3 байт собирается по одному.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "debug_out.h"

/* --- локальные объявления того, что нужно из InterruptControl.c ---
 * (в проекте нет отдельного InterruptControl.h — остальные модули тоже
 * просто дают свои extern-объявления по месту использования). */
typedef void (*isr_handler_t)(uint64_t num, uint64_t err, void *frame);
extern int interrupt_register_handler(uint16_t vector, isr_handler_t handler);
extern void apic_send_eoi(uint8_t vector);
/* ioapic_init() при старте маскирует ВСЕ линии IOAPIC — без явного
 * ioapic_route_irq() прерывание IRQ12 никогда не дойдёт до CPU, даже
 * если сам i8042 его генерирует и IDT-гейт настроен. */
extern void ioapic_route_irq(uint8_t irq, uint8_t vector,
                             uint32_t dest_lapic_id, bool level, bool low_pol);

/* --- из ui_extra.c / Framebuffer.h --- */
extern void     ui_set_mouse_pos(int32_t x, int32_t y);
extern uint32_t fb_width(void);
extern uint32_t fb_height(void);
extern int      fb_get_mode(void);
#define FB_MODE_LINEAR 2

/* --- I/O порты контроллера клавиатуры/мыши (i8042) --- */
#define PS2_DATA    0x60
#define PS2_STATUS  0x64
#define PS2_CMD     0x64

#define PS2_STATUS_OUT_FULL   0x01   /* можно читать 0x60 */
#define PS2_STATUS_IN_FULL    0x02   /* нельзя писать пока не сброшен */

static inline void ps2_outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t ps2_inb(uint16_t port) {
    uint8_t r;
    __asm__ volatile ("inb %1, %0" : "=a"(r) : "Nd"(port));
    return r;
}

static int ps2_wait_write(void) {
    for (uint32_t t = 0; t < 100000; t++)
        if (!(ps2_inb(PS2_STATUS) & PS2_STATUS_IN_FULL)) return 0;
    return -1;
}
static int ps2_wait_read(void) {
    for (uint32_t t = 0; t < 100000; t++)
        if (ps2_inb(PS2_STATUS) & PS2_STATUS_OUT_FULL) return 0;
    return -1;
}

/* Отправить байт мыши (aux-устройству), с ожиданием ACK (0xFA) */
static int ps2_mouse_write(uint8_t val) {
    if (ps2_wait_write() < 0) return -1;
    ps2_outb(PS2_CMD, 0xD4);           /* следующий байт — для aux/mouse */
    if (ps2_wait_write() < 0) return -1;
    ps2_outb(PS2_DATA, val);

    if (ps2_wait_read() < 0) return -1;
    uint8_t ack = ps2_inb(PS2_DATA);
    return (ack == 0xFA) ? 0 : -1;
}

/* --- состояние сборки пакета --- */
static uint8_t  g_packet[3];
static int      g_packet_idx = 0;
static uint8_t  g_buttons = 0;

static int32_t g_pos_x = 100, g_pos_y = 100;
static int     g_bounds_known = 0;

uint8_t ps2_mouse_buttons(void) { return g_buttons; }

static void ps2_mouse_isr(uint64_t num, uint64_t err, void *frame) {
    (void)num; (void)err; (void)frame;

    uint8_t data = ps2_inb(PS2_DATA);

    /* byte0 всегда имеет bit3=1 — если "потеряли синхронизацию"
     * (например пропустили байт), выравниваемся заново. */
    if (g_packet_idx == 0 && !(data & 0x08)) return;

    g_packet[g_packet_idx++] = data;
    if (g_packet_idx < 3) return;
    g_packet_idx = 0;

    uint8_t flags = g_packet[0];
    int32_t dx = (int32_t)(uint8_t)g_packet[1];
    int32_t dy = (int32_t)(uint8_t)g_packet[2];

    if (flags & 0x10) dx -= 256;   /* X sign */
    if (flags & 0x20) dy -= 256;   /* Y sign */
    /* overflow (bit6/7) — пакет ненадёжен, дельту игнорируем */
    if (flags & 0xC0) { dx = 0; dy = 0; }

    g_buttons = flags & 0x07;

    if (fb_get_mode() == FB_MODE_LINEAR) {
        if (!g_bounds_known) g_bounds_known = 1;

        int32_t max_x = (int32_t)fb_width()  - 1;
        int32_t max_y = (int32_t)fb_height() - 1;
        if (max_x < 0) max_x = 0;
        if (max_y < 0) max_y = 0;

        g_pos_x += dx;
        g_pos_y -= dy;   /* PS/2 Y+ = вверх, экран Y+ = вниз */

        if (g_pos_x < 0) g_pos_x = 0;
        if (g_pos_y < 0) g_pos_y = 0;
        if (g_pos_x > max_x) g_pos_x = max_x;
        if (g_pos_y > max_y) g_pos_y = max_y;

        ui_set_mouse_pos(g_pos_x, g_pos_y);
    }
}

int ps2_mouse_init(void) {
    DBG_MSG("MS", "ps2_mouse_init: start");

    /* 1. Включаем aux-порт (мышь) */
    if (ps2_wait_write() < 0) { DBG_MSG("MS", "ctrl busy (enable aux)"); return -1; }
    ps2_outb(PS2_CMD, 0xA8);

    /* 2. Читаем configuration byte, включаем IRQ12 (bit1) и clock мыши
     *    (сбрасываем bit5), пишем обратно. */
    if (ps2_wait_write() < 0) return -1;
    ps2_outb(PS2_CMD, 0x20);           /* read config byte */
    if (ps2_wait_read() < 0) return -1;
    uint8_t cfg = ps2_inb(PS2_DATA);

    cfg |= 0x02;    /* enable IRQ12 (mouse interrupt) */
    cfg &= ~0x20;   /* enable mouse clock */

    if (ps2_wait_write() < 0) return -1;
    ps2_outb(PS2_CMD, 0x60);           /* write config byte */
    if (ps2_wait_write() < 0) return -1;
    ps2_outb(PS2_DATA, cfg);

    /* 3. Сброс мыши */
    if (ps2_mouse_write(0xFF) < 0) { DBG_MSG("MS", "reset: no ACK"); return -1; }
    if (ps2_wait_read() < 0) { DBG_MSG("MS", "reset: no self-test byte"); return -1; }
    uint8_t self_test = ps2_inb(PS2_DATA);           /* ожидаем 0xAA */
    if (ps2_wait_read() == 0) (void)ps2_inb(PS2_DATA); /* device id, обычно 0x00 */
    DBG_VAL("MS", "reset: self_test", (uint64_t)self_test);

    /* 4. Настройки по умолчанию + включить передачу данных */
    if (ps2_mouse_write(0xF6) < 0) { DBG_MSG("MS", "set defaults: no ACK"); return -1; }
    if (ps2_mouse_write(0xF4) < 0) { DBG_MSG("MS", "enable reporting: no ACK"); return -1; }

    /* 5. Регистрируем обработчик IRQ12 (вектор 0x2C = IRQ_BASE+12) */
    if (interrupt_register_handler(0x2C, ps2_mouse_isr) != 0) {
        DBG_MSG("MS", "interrupt_register_handler failed");
        return -1;
    }

    /* ioapic_init() маскирует все линии при старте — явно разблокируем
     * и маршрутизируем IRQ12 на CPU0 (edge-triggered, active-high —
     * стандарт для legacy ISA IRQ, как и PS/2). Без этого шага прерывание
     * от мыши физически не дойдёт до ядра, даже с рабочим IDT-гейтом. */
    ioapic_route_irq(12, 0x2C, 0, false, false);

    DBG_MSG("MS", "ps2_mouse_init: OK, mouse enabled");
    return 0;
}
