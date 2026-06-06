/* debug_out.h — вывод в COM1 (QEMU: -serial stdio), без libc */
#ifndef DEBUG_OUT_H
#define DEBUG_OUT_H

#include <stdint.h>
#include <stddef.h>

static inline void dbg_outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t dbg_inb(uint16_t port) {
    uint8_t r;
    __asm__ volatile ("inb %1, %0" : "=a"(r) : "Nd"(port));
    return r;
}

static inline void dbg_putc(char c) {
    /* Только COM1 — не дублировать с debugcon на одном chardev */
    uint32_t timeout = 0x100000;
    while ((dbg_inb(0x3F8 + 5) & 0x20) == 0 && timeout--)
        ;
    dbg_outb(0x3F8, (uint8_t)c);
}

static inline void dbg_puts(const char *s) {
    while (*s) {
        if (*s == '\n')
            dbg_putc('\r');
        dbg_putc(*s++);
    }
}

static inline void dbg_hex64(uint64_t v) {
    static const char hex[] = "0123456789ABCDEF";
    dbg_puts("0x");
    for (int i = 60; i >= 0; i -= 4)
        dbg_putc(hex[(v >> (unsigned)i) & 0xF]);
}

static inline void dbg_dec64(uint64_t v) {
    char buf[24];
    int n = 0;
    if (v == 0) {
        dbg_putc('0');
        return;
    }
    while (v > 0) {
        buf[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n > 0)
        dbg_putc(buf[--n]);
}

/* [TAG] message (строка msg — литерал) */
#define DBG_MSG(tag, msg) do { dbg_puts("[" tag "] " msg "\n"); } while(0)

/* [TAG] name=0x... */
#define DBG_VAL(tag, name, val) do { \
    dbg_puts("[" tag "] " name "="); \
    dbg_hex64((uint64_t)(val)); \
    dbg_puts("\n"); \
} while(0)

/* Остановка с сообщением (видно в терминале QEMU -serial stdio) */
#define DBG_PANIC(tag, msg) do { \
    dbg_puts("\n*** PANIC [" tag "] "); \
    dbg_puts(msg); \
    dbg_puts(" ***\n"); \
    for (;;) __asm__ volatile ("hlt"); \
} while(0)

#endif /* DEBUG_OUT_H */
