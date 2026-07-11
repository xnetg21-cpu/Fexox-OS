/*
 * Framebuffer.c — драйвер экрана FEXOS
 *
 * Зависимости ядра:
 *   MemoryControl: kmalloc / kfree, DIRECT_MAP_OFFSET
 *   klibc:         memset / memcpy
 *   debug_out.h:   DBG_MSG / DBG_VAL
 */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "Framebuffer.h"
#include "png.h"
#include "debug_out.h"
#include "AcpiControl.h"

/* =========================================================================
 * Внешние зависимости ядра
 * ========================================================================= */
extern void *kmalloc(uint64_t size);
extern void  kfree(void *ptr);

/* DIRECT_MAP_OFFSET из MemoryControl.c:
   физический адрес pa → виртуальный = pa + DIRECT_MAP_OFFSET */
#define DIRECT_MAP_OFFSET 0xFFFF880000000000ULL

/* klibc */
extern void *memset(void *dst, int c, size_t n);
extern void *memcpy(void *dst, const void *src, size_t n);

/* =========================================================================
 * Встроенный шрифт 8×16 (VGA BIOS ROM font, 96 символов: 0x20–0x7F)
 *
 * Каждый символ — 16 байт (по одному на строку).
 * Бит 7 = левый пиксель, бит 0 = правый.
 * ========================================================================= */
static const uint8_t g_font8x16[96][16] = {
    /* 0x20 space */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 0x21 ! */
    {0x00,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x00,0x18,0x18,0x00,0x00,0x00,0x00,0x00},
    /* 0x22 " */
    {0x00,0x66,0x66,0x66,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 0x23 # */
    {0x00,0x36,0x36,0x7F,0x36,0x36,0x36,0x7F,0x36,0x36,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 0x24 $ */
    {0x18,0x18,0x7C,0xC6,0xC2,0xC0,0x7C,0x06,0x06,0x86,0xC6,0x7C,0x18,0x18,0x00,0x00},
    /* 0x25 % */
    {0x00,0x00,0xC2,0xC6,0x0C,0x18,0x30,0x60,0xC6,0x86,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 0x26 & */
    {0x00,0x38,0x6C,0x6C,0x38,0x76,0xDC,0xCC,0xCC,0x76,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 0x27 ' */
    {0x00,0x30,0x30,0x30,0x60,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 0x28 ( */
    {0x00,0x0C,0x18,0x30,0x60,0x60,0x60,0x60,0x60,0x30,0x18,0x0C,0x00,0x00,0x00,0x00},
    /* 0x29 ) */
    {0x00,0x60,0x30,0x18,0x0C,0x0C,0x0C,0x0C,0x0C,0x18,0x30,0x60,0x00,0x00,0x00,0x00},
    /* 0x2A * */
    {0x00,0x00,0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 0x2B + */
    {0x00,0x00,0x00,0x18,0x18,0x18,0xFF,0x18,0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 0x2C , */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x18,0x30,0x00,0x00,0x00},
    /* 0x2D - */
    {0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 0x2E . */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x00,0x00},
    /* 0x2F / */
    {0x00,0x00,0x02,0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 0x30 0 */
    {0x00,0x3C,0x66,0xC3,0xC3,0xDB,0xDB,0xC3,0xC3,0x66,0x3C,0x00,0x00,0x00,0x00,0x00},
    /* 0x31 1 */
    {0x00,0x18,0x38,0x58,0x18,0x18,0x18,0x18,0x18,0x18,0x7E,0x00,0x00,0x00,0x00,0x00},
    /* 0x32 2 */
    {0x00,0x7C,0xC6,0x06,0x0C,0x18,0x30,0x60,0xC0,0xC6,0xFE,0x00,0x00,0x00,0x00,0x00},
    /* 0x33 3 */
    {0x00,0x7C,0xC6,0x06,0x06,0x3C,0x06,0x06,0x06,0xC6,0x7C,0x00,0x00,0x00,0x00,0x00},
    /* 0x34 4 */
    {0x00,0x0C,0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x0C,0x0C,0x1E,0x00,0x00,0x00,0x00,0x00},
    /* 0x35 5 */
    {0x00,0xFE,0xC0,0xC0,0xC0,0xFC,0x06,0x06,0x06,0xC6,0x7C,0x00,0x00,0x00,0x00,0x00},
    /* 0x36 6 */
    {0x00,0x38,0x60,0xC0,0xC0,0xFC,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00,0x00},
    /* 0x37 7 */
    {0x00,0xFE,0xC6,0x06,0x06,0x0C,0x18,0x30,0x30,0x30,0x30,0x00,0x00,0x00,0x00,0x00},
    /* 0x38 8 */
    {0x00,0x7C,0xC6,0xC6,0xC6,0x7C,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00,0x00},
    /* 0x39 9 */
    {0x00,0x7C,0xC6,0xC6,0xC6,0x7E,0x06,0x06,0x06,0x0C,0x78,0x00,0x00,0x00,0x00,0x00},
    /* 0x3A : */
    {0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 0x3B ; */
    {0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x18,0x18,0x18,0x30,0x00,0x00,0x00,0x00},
    /* 0x3C < */
    {0x00,0x00,0x06,0x0C,0x18,0x30,0x60,0x30,0x18,0x0C,0x06,0x00,0x00,0x00,0x00,0x00},
    /* 0x3D = */
    {0x00,0x00,0x00,0x00,0xFF,0x00,0x00,0xFF,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 0x3E > */
    {0x00,0x00,0x60,0x30,0x18,0x0C,0x06,0x0C,0x18,0x30,0x60,0x00,0x00,0x00,0x00,0x00},
    /* 0x3F ? */
    {0x00,0x7C,0xC6,0xC6,0x0C,0x18,0x18,0x18,0x00,0x18,0x18,0x00,0x00,0x00,0x00,0x00},
    /* 0x40 @ */
    {0x00,0x7C,0xC6,0xC6,0xDE,0xDE,0xDE,0xDC,0xC0,0xC4,0x7C,0x00,0x00,0x00,0x00,0x00},
    /* 0x41 A */
    {0x00,0x10,0x38,0x6C,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00,0x00},
    /* 0x42 B */
    {0x00,0xFC,0x66,0x66,0x66,0x7C,0x66,0x66,0x66,0x66,0xFC,0x00,0x00,0x00,0x00,0x00},
    /* 0x43 C */
    {0x00,0x3C,0x66,0xC2,0xC0,0xC0,0xC0,0xC0,0xC2,0x66,0x3C,0x00,0x00,0x00,0x00,0x00},
    /* 0x44 D */
    {0x00,0xF8,0x6C,0x66,0x66,0x66,0x66,0x66,0x66,0x6C,0xF8,0x00,0x00,0x00,0x00,0x00},
    /* 0x45 E */
    {0x00,0xFE,0x66,0x62,0x68,0x78,0x68,0x60,0x62,0x66,0xFE,0x00,0x00,0x00,0x00,0x00},
    /* 0x46 F */
    {0x00,0xFE,0x66,0x62,0x68,0x78,0x68,0x60,0x60,0x60,0xF0,0x00,0x00,0x00,0x00,0x00},
    /* 0x47 G */
    {0x00,0x3C,0x66,0xC2,0xC0,0xC0,0xDE,0xC6,0xC6,0x66,0x3A,0x00,0x00,0x00,0x00,0x00},
    /* 0x48 H */
    {0x00,0xC6,0xC6,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00,0x00},
    /* 0x49 I */
    {0x00,0x3C,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00,0x00},
    /* 0x4A J */
    {0x00,0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0xCC,0xCC,0xCC,0x78,0x00,0x00,0x00,0x00,0x00},
    /* 0x4B K */
    {0x00,0xE6,0x66,0x6C,0x6C,0x78,0x78,0x6C,0x66,0x66,0xE6,0x00,0x00,0x00,0x00,0x00},
    /* 0x4C L */
    {0x00,0xF0,0x60,0x60,0x60,0x60,0x60,0x60,0x62,0x66,0xFE,0x00,0x00,0x00,0x00,0x00},
    /* 0x4D M */
    {0x00,0xC6,0xEE,0xFE,0xFE,0xD6,0xC6,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00,0x00},
    /* 0x4E N */
    {0x00,0xC6,0xE6,0xF6,0xFE,0xDE,0xCE,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00,0x00},
    /* 0x4F O */
    {0x00,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00,0x00},
    /* 0x50 P */
    {0x00,0xFC,0x66,0x66,0x66,0x7C,0x60,0x60,0x60,0x60,0xF0,0x00,0x00,0x00,0x00,0x00},
    /* 0x51 Q */
    {0x00,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xD6,0xDE,0x7C,0x0C,0x0E,0x00,0x00,0x00},
    /* 0x52 R */
    {0x00,0xFC,0x66,0x66,0x66,0x7C,0x6C,0x66,0x66,0x66,0xE6,0x00,0x00,0x00,0x00,0x00},
    /* 0x53 S */
    {0x00,0x7C,0xC6,0xC6,0x60,0x38,0x0C,0x06,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00,0x00},
    /* 0x54 T */
    {0x00,0xFF,0xDB,0x99,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00,0x00},
    /* 0x55 U */
    {0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00,0x00},
    /* 0x56 V */
    {0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x10,0x00,0x00,0x00,0x00,0x00},
    /* 0x57 W */
    {0x00,0xC6,0xC6,0xC6,0xC6,0xD6,0xD6,0xD6,0xFE,0xEE,0xC6,0x00,0x00,0x00,0x00,0x00},
    /* 0x58 X */
    {0x00,0xC6,0xC6,0x6C,0x6C,0x38,0x38,0x6C,0x6C,0xC6,0xC6,0x00,0x00,0x00,0x00,0x00},
    /* 0x59 Y */
    {0x00,0xCC,0xCC,0xCC,0xCC,0x78,0x30,0x30,0x30,0x30,0x78,0x00,0x00,0x00,0x00,0x00},
    /* 0x5A Z */
    {0x00,0xFE,0xC6,0x86,0x0C,0x18,0x30,0x60,0xC2,0xC6,0xFE,0x00,0x00,0x00,0x00,0x00},
    /* 0x5B [ */
    {0x00,0x3C,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x3C,0x00,0x00,0x00,0x00,0x00},
    /* 0x5C \ */
    {0x00,0x00,0x80,0xC0,0x60,0x30,0x18,0x0C,0x06,0x02,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 0x5D ] */
    {0x00,0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00,0x00,0x00,0x00,0x00},
    /* 0x5E ^ */
    {0x10,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 0x5F _ */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0x00,0x00,0x00,0x00},
    /* 0x60 ` */
    {0x00,0x30,0x18,0x0C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 0x61 a */
    {0x00,0x00,0x00,0x00,0x78,0x0C,0x7C,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00,0x00,0x00},
    /* 0x62 b */
    {0x00,0xE0,0x60,0x60,0x78,0x6C,0x66,0x66,0x66,0x66,0x7C,0x00,0x00,0x00,0x00,0x00},
    /* 0x63 c */
    {0x00,0x00,0x00,0x00,0x7C,0xC6,0xC0,0xC0,0xC0,0xC6,0x7C,0x00,0x00,0x00,0x00,0x00},
    /* 0x64 d */
    {0x00,0x1C,0x0C,0x0C,0x3C,0x6C,0xCC,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00,0x00,0x00},
    /* 0x65 e */
    {0x00,0x00,0x00,0x00,0x7C,0xC6,0xFE,0xC0,0xC0,0xC6,0x7C,0x00,0x00,0x00,0x00,0x00},
    /* 0x66 f */
    {0x00,0x1C,0x36,0x32,0x30,0x78,0x30,0x30,0x30,0x30,0x78,0x00,0x00,0x00,0x00,0x00},
    /* 0x67 g */
    {0x00,0x00,0x00,0x00,0x76,0xCC,0xCC,0xCC,0xCC,0x7C,0x0C,0xCC,0x78,0x00,0x00,0x00},
    /* 0x68 h */
    {0x00,0xE0,0x60,0x60,0x6C,0x76,0x66,0x66,0x66,0x66,0xE6,0x00,0x00,0x00,0x00,0x00},
    /* 0x69 i */
    {0x00,0x18,0x18,0x00,0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00,0x00},
    /* 0x6A j */
    {0x00,0x06,0x06,0x00,0x0E,0x06,0x06,0x06,0x06,0x06,0x66,0x66,0x3C,0x00,0x00,0x00},
    /* 0x6B k */
    {0x00,0xE0,0x60,0x60,0x66,0x6C,0x78,0x78,0x6C,0x66,0xE6,0x00,0x00,0x00,0x00,0x00},
    /* 0x6C l */
    {0x00,0x38,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00,0x00},
    /* 0x6D m */
    {0x00,0x00,0x00,0x00,0xEC,0xFE,0xD6,0xD6,0xD6,0xD6,0xC6,0x00,0x00,0x00,0x00,0x00},
    /* 0x6E n */
    {0x00,0x00,0x00,0x00,0xDC,0x66,0x66,0x66,0x66,0x66,0x66,0x00,0x00,0x00,0x00,0x00},
    /* 0x6F o */
    {0x00,0x00,0x00,0x00,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00,0x00},
    /* 0x70 p */
    {0x00,0x00,0x00,0x00,0xDC,0x66,0x66,0x66,0x66,0x7C,0x60,0x60,0xF0,0x00,0x00,0x00},
    /* 0x71 q */
    {0x00,0x00,0x00,0x00,0x76,0xCC,0xCC,0xCC,0xCC,0x7C,0x0C,0x0C,0x1E,0x00,0x00,0x00},
    /* 0x72 r */
    {0x00,0x00,0x00,0x00,0xDC,0x76,0x66,0x60,0x60,0x60,0xF0,0x00,0x00,0x00,0x00,0x00},
    /* 0x73 s */
    {0x00,0x00,0x00,0x00,0x7C,0xC6,0x60,0x38,0x0C,0xC6,0x7C,0x00,0x00,0x00,0x00,0x00},
    /* 0x74 t */
    {0x00,0x10,0x30,0x30,0xFC,0x30,0x30,0x30,0x30,0x36,0x1C,0x00,0x00,0x00,0x00,0x00},
    /* 0x75 u */
    {0x00,0x00,0x00,0x00,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00,0x00,0x00},
    /* 0x76 v */
    {0x00,0x00,0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x00,0x00,0x00,0x00,0x00},
    /* 0x77 w */
    {0x00,0x00,0x00,0x00,0xC6,0xC6,0xC6,0xD6,0xD6,0xFE,0x6C,0x00,0x00,0x00,0x00,0x00},
    /* 0x78 x */
    {0x00,0x00,0x00,0x00,0xC6,0x6C,0x38,0x38,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00,0x00},
    /* 0x79 y */
    {0x00,0x00,0x00,0x00,0xCC,0xCC,0xCC,0xCC,0xCC,0x7C,0x0C,0xCC,0x78,0x00,0x00,0x00},
    /* 0x7A z */
    {0x00,0x00,0x00,0x00,0xFE,0xCC,0x18,0x30,0x60,0xC6,0xFE,0x00,0x00,0x00,0x00,0x00},
    /* 0x7B { */
    {0x00,0x0E,0x18,0x18,0x18,0x70,0x18,0x18,0x18,0x18,0x0E,0x00,0x00,0x00,0x00,0x00},
    /* 0x7C | */
    {0x00,0x18,0x18,0x18,0x18,0x00,0x00,0x18,0x18,0x18,0x18,0x00,0x00,0x00,0x00,0x00},
    /* 0x7D } */
    {0x00,0x70,0x18,0x18,0x18,0x0E,0x18,0x18,0x18,0x18,0x70,0x00,0x00,0x00,0x00,0x00},
    /* 0x7E ~ */
    {0x00,0x76,0xDC,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 0x7F DEL (заглушка) */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
};

/* =========================================================================
 * Внутреннее состояние драйвера
 * ========================================================================= */
typedef struct {
    int      mode;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint8_t  bpp;
    uint8_t  bytes_pp;

    uint8_t *front;
    uint8_t *back;

    uint8_t  r_shift;
    uint8_t  g_shift;
    uint8_t  b_shift;
} fb_state_t;

static fb_state_t g_fb;

static uint8_t mask_to_shift(uint32_t mask) {
    if (!mask) return 0;
    uint8_t s = 0;
    while ((mask & 1) == 0) { mask >>= 1; s++; }
    return s;
}

static inline uint32_t rgb_to_native(fb_color_t c) {
    uint8_t r = (c >> 16) & 0xFF;
    uint8_t g = (c >>  8) & 0xFF;
    uint8_t b = (c      ) & 0xFF;
    return ((uint32_t)r << g_fb.r_shift) |
           ((uint32_t)g << g_fb.g_shift) |
           ((uint32_t)b << g_fb.b_shift);
}

static inline uint8_t *draw_buf(void) {
    return g_fb.back ? g_fb.back : g_fb.front;
}

/* =========================================================================
 * fb_init
 * ========================================================================= */
int fb_init(const fb_info_t *info) {
    for (int i = 0; i < (int)sizeof(fb_state_t); i++)
        ((uint8_t *)&g_fb)[i] = 0;

    if (!info || info->mode == FB_MODE_VGA || info->mode == FB_MODE_NONE) {
        g_fb.mode = FB_MODE_VGA;
        DBG_MSG("FB", "VGA text mode fallback");
        return FB_MODE_VGA;
    }

    if (info->mode == FB_MODE_LINEAR && info->phys_addr && info->width && info->height) {
        g_fb.mode      = FB_MODE_LINEAR;
        g_fb.width     = info->width;
        g_fb.height    = info->height;
        g_fb.pitch     = info->pitch  ? info->pitch  : info->width * (info->bpp / 8);
        g_fb.bpp       = info->bpp    ? info->bpp    : 32;
        g_fb.bytes_pp  = g_fb.bpp / 8;

        g_fb.front = (uint8_t *)(uintptr_t)(info->phys_addr + DIRECT_MAP_OFFSET);

        if (info->red_mask || info->green_mask || info->blue_mask) {
            g_fb.r_shift = mask_to_shift(info->red_mask);
            g_fb.g_shift = mask_to_shift(info->green_mask);
            g_fb.b_shift = mask_to_shift(info->blue_mask);
        } else {
            g_fb.r_shift = 16;
            g_fb.g_shift = 8;
            g_fb.b_shift = 0;
        }

        DBG_MSG("FB", "linear framebuffer init");
        DBG_VAL("FB", "width",  g_fb.width);
        DBG_VAL("FB", "height", g_fb.height);
        DBG_VAL("FB", "pitch",  g_fb.pitch);
        DBG_VAL("FB", "front",  (uint64_t)(uintptr_t)g_fb.front);

        return FB_MODE_LINEAR;
    }

    g_fb.mode = FB_MODE_VGA;
    return FB_MODE_VGA;
}

int      fb_get_mode(void)   { return g_fb.mode; }
uint32_t fb_width(void)      { return g_fb.width; }
uint32_t fb_height(void)     { return g_fb.height; }

/* =========================================================================
 * Double-buffering
 * ========================================================================= */
int fb_enable_double_buffer(void) {
    if (g_fb.mode != FB_MODE_LINEAR) return -1;
    if (g_fb.back) return 0;

    uint64_t size = (uint64_t)g_fb.pitch * g_fb.height;
    g_fb.back = (uint8_t *)kmalloc(size);
    if (!g_fb.back) { DBG_MSG("FB", "double-buffer: kmalloc failed"); return -1; }

    /* Не читаем g_fb.front — физический framebuffer может лежать за пределами
     * DIRECT_MAP (MMIO-регион, EFI-reserved), чтение оттуда вызовет page fault.
     * Обнуляем back-buffer: ui_draw_desktop всё равно перерисует весь экран. */
    memset(g_fb.back, 0, size);
    DBG_MSG("FB", "double-buffer enabled");
    return 0;
}

void fb_disable_double_buffer(void) {
    if (g_fb.back) { kfree(g_fb.back); g_fb.back = 0; }
}

/*
 * memcpy_nt — копирование в MMIO-регион через non-temporal stores (movnti).
 *
 * Обычный memcpy на UC/WC GOP framebuffer работает крайне медленно:
 * процессор не может объединять UC-записи в burst, каждая запись
 * уходит на шину по отдельности. На буфере 4+ МБ это выглядит как
 * зависание на несколько секунд.
 *
 * movnti обходит кэш и пишет напрямую через write-combining буфер:
 * скорость записи вырастает в 10-50× по сравнению с UC memcpy.
 * sfence гарантирует, что все NT-записи видны на шине до возврата.
 *
 * Требования: x86-64 (movnti поддерживается с Pentium 4 / Athlon 64).
 */
static void memcpy_nt(void *dst, const void *src, size_t n) {
    uint64_t       *d = (uint64_t *)dst;
    const uint64_t *s = (const uint64_t *)src;
    size_t count = n / 8;

    for (size_t i = 0; i < count; i++) {
        __asm__ volatile (
            "movnti %1, %0"
            : "=m"(d[i])
            : "r"(s[i])
            :
        );
    }

    /* sfence: сбрасывает write-combining буфер, делает NT-записи видимыми */
    __asm__ volatile ("sfence" ::: "memory");

    /* Хвост (если pitch*height не кратен 8 байтам) */
    size_t tail = n & 7u;
    if (tail) {
        uint8_t       *db = (uint8_t *)(d + count);
        const uint8_t *sb = (const uint8_t *)(s + count);
        for (size_t i = 0; i < tail; i++) db[i] = sb[i];
    }
}

void fb_flip(void) {
    if (!g_fb.back || !g_fb.front) return;
    /* NT-stores вместо обычного memcpy: критично для GOP MMIO-региона,
     * который отображён как UC или WC. Обычный memcpy там в 10-50 раз
     * медленнее и вызывает зависание при копировании 4+ МБ. */
    memcpy_nt(g_fb.front, g_fb.back, (size_t)g_fb.pitch * g_fb.height);
}

void *fb_back_ptr(void) { return g_fb.back; }

/* =========================================================================
 * Примитивы рисования
 * ========================================================================= */

void fb_put_pixel(int32_t x, int32_t y, fb_color_t color) {
    if (g_fb.mode != FB_MODE_LINEAR) return;
    if ((uint32_t)x >= g_fb.width || (uint32_t)y >= g_fb.height) return;

    uint32_t native = rgb_to_native(color);
    uint8_t *dst = draw_buf() + (uint64_t)y * g_fb.pitch + (uint64_t)x * g_fb.bytes_pp;

    switch (g_fb.bytes_pp) {
        case 4: dst[3] = (uint8_t)(native >> 24); /* fall through */
        case 3: dst[2] = (uint8_t)(native >> 16); /* fall through */
        case 2: dst[1] = (uint8_t)(native >>  8); /* fall through */
        case 1: dst[0] = (uint8_t)(native      ); break;
    }
}

fb_color_t fb_get_pixel(int32_t x, int32_t y) {
    if (g_fb.mode != FB_MODE_LINEAR) return 0;
    if ((uint32_t)x >= g_fb.width || (uint32_t)y >= g_fb.height) return 0;

    const uint8_t *src = draw_buf() + (uint64_t)y * g_fb.pitch + (uint64_t)x * g_fb.bytes_pp;
    uint32_t native = 0;
    switch (g_fb.bytes_pp) {
        case 4: native |= (uint32_t)src[3] << 24; /* fall through */
        case 3: native |= (uint32_t)src[2] << 16; /* fall through */
        case 2: native |= (uint32_t)src[1] <<  8; /* fall through */
        case 1: native |= (uint32_t)src[0];       break;
    }
    uint8_t r = (uint8_t)((native >> g_fb.r_shift) & 0xFF);
    uint8_t g = (uint8_t)((native >> g_fb.g_shift) & 0xFF);
    uint8_t b = (uint8_t)((native >> g_fb.b_shift) & 0xFF);
    return ((fb_color_t)r << 16) | ((fb_color_t)g << 8) | b;
}

void fb_hline(int32_t x, int32_t y, uint32_t len, fb_color_t color) {
    if (g_fb.mode != FB_MODE_LINEAR) return;
    if ((uint32_t)y >= g_fb.height) return;
    if (x < 0) { if ((uint32_t)(-x) >= len) return; len += x; x = 0; }
    if ((uint32_t)x + len > g_fb.width) len = g_fb.width - (uint32_t)x;
    if (!len) return;

    uint32_t native = rgb_to_native(color);
    uint8_t *row = draw_buf() + (uint64_t)y * g_fb.pitch + (uint64_t)x * g_fb.bytes_pp;

    if (g_fb.bytes_pp == 4) {
        uint32_t *p = (uint32_t *)row;
        for (uint32_t i = 0; i < len; i++) p[i] = native;
    } else {
        for (uint32_t i = 0; i < len; i++) {
            uint8_t *p = row + i * g_fb.bytes_pp;
            switch (g_fb.bytes_pp) {
                case 3: p[2] = (uint8_t)(native >> 16); /* fall through */
                case 2: p[1] = (uint8_t)(native >>  8); /* fall through */
                case 1: p[0] = (uint8_t)(native      ); break;
            }
        }
    }
}

void fb_vline(int32_t x, int32_t y, uint32_t len, fb_color_t color) {
    if (g_fb.mode != FB_MODE_LINEAR) return;
    if ((uint32_t)x >= g_fb.width) return;
    if (y < 0) { if ((uint32_t)(-y) >= len) return; len += y; y = 0; }
    if ((uint32_t)y + len > g_fb.height) len = g_fb.height - (uint32_t)y;
    for (uint32_t i = 0; i < len; i++)
        fb_put_pixel(x, (int32_t)((uint32_t)y + i), color);
}

void fb_fill_rect(int32_t x, int32_t y, uint32_t w, uint32_t h, fb_color_t color) {
    if (g_fb.mode != FB_MODE_LINEAR) return;
    if (x < 0) { if ((uint32_t)(-x) >= w) return; w += x; x = 0; }
    if (y < 0) { if ((uint32_t)(-y) >= h) return; h += y; y = 0; }
    if ((uint32_t)x >= g_fb.width || (uint32_t)y >= g_fb.height) return;
    if ((uint32_t)x + w > g_fb.width)  w = g_fb.width  - (uint32_t)x;
    if ((uint32_t)y + h > g_fb.height) h = g_fb.height - (uint32_t)y;
    if (!w || !h) return;

    for (uint32_t row = 0; row < h; row++)
        fb_hline(x, (int32_t)((uint32_t)y + row), w, color);
}

void fb_draw_rect(int32_t x, int32_t y, uint32_t w, uint32_t h,
                  fb_color_t color, uint32_t t) {
    if (!t) t = 1;
    fb_fill_rect(x,                  y,                  w, t, color);
    fb_fill_rect(x,                  y + (int32_t)(h-t), w, t, color);
    fb_fill_rect(x,                  y,                  t, h, color);
    fb_fill_rect(x + (int32_t)(w-t), y,                  t, h, color);
}

void fb_clear(fb_color_t color) {
    fb_fill_rect(0, 0, g_fb.width, g_fb.height, color);
}

void fb_blit(int32_t dx, int32_t dy, int32_t sx, int32_t sy, uint32_t w, uint32_t h) {
    if (g_fb.mode != FB_MODE_LINEAR) return;
    uint8_t *buf = draw_buf();
    for (uint32_t row = 0; row < h; row++) {
        int32_t sr = (int32_t)((uint32_t)sy + row);
        int32_t dr = (int32_t)((uint32_t)dy + row);
        if ((uint32_t)sr >= g_fb.height || (uint32_t)dr >= g_fb.height) continue;
        uint8_t *sp = buf + (uint64_t)(uint32_t)sr * g_fb.pitch + (uint64_t)(uint32_t)sx * g_fb.bytes_pp;
        uint8_t *dp = buf + (uint64_t)(uint32_t)dr * g_fb.pitch + (uint64_t)(uint32_t)dx * g_fb.bytes_pp;
        uint64_t nbytes = (uint64_t)w * g_fb.bytes_pp;
        extern void *memmove(void *, const void *, size_t);
        memmove(dp, sp, (size_t)nbytes);
    }
}

void fb_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, fb_color_t color) {
    int32_t dx =  (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int32_t dy = -((y1 > y0) ? (y1 - y0) : (y0 - y1));
    int32_t sx = (x0 < x1) ? 1 : -1;
    int32_t sy = (y0 < y1) ? 1 : -1;
    int32_t err = dx + dy;

    while (1) {
        fb_put_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int32_t e2 = err * 2;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* =========================================================================
 * Текст
 * ========================================================================= */
void fb_draw_char(int32_t x, int32_t y, char c, fb_color_t fg, fb_color_t bg) {
    if (g_fb.mode != FB_MODE_LINEAR) return;

    uint8_t idx = (uint8_t)c;
    if (idx < 0x20 || idx > 0x7F) idx = '?';
    const uint8_t *glyph = g_font8x16[idx - 0x20];

    bool transparent = (bg == FB_TRANSPARENT_BG);

    for (int row = 0; row < FB_FONT_H; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < FB_FONT_W; col++) {
            bool set = (bits >> (7 - col)) & 1;
            if (set)
                fb_put_pixel(x + col, y + row, fg);
            else if (!transparent)
                fb_put_pixel(x + col, y + row, bg);
        }
    }
}

int32_t fb_draw_string(int32_t x, int32_t y, const char *str,
                       fb_color_t fg, fb_color_t bg) {
    while (*str) {
        fb_draw_char(x, y, *str, fg, bg);
        x += FB_FONT_W;
        str++;
    }
    return x;
}

/* =========================================================================
 * fb_console
 * ========================================================================= */
typedef struct {
    uint32_t   ox, oy;
    uint32_t   ow, oh;
    uint32_t   cols;
    uint32_t   rows;
    uint32_t   cur_col;
    uint32_t   cur_row;
    fb_color_t fg, bg;
} fbcon_t;

static fbcon_t g_con;

void fbcon_init(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                fb_color_t fg, fb_color_t bg) {
    g_con.ox      = x;
    g_con.oy      = y;
    g_con.ow      = w;
    g_con.oh      = h;
    g_con.cols    = w / FB_FONT_W;
    g_con.rows    = h / FB_FONT_H;
    g_con.cur_col = 0;
    g_con.cur_row = 0;
    g_con.fg      = fg;
    g_con.bg      = bg;

    if (g_fb.mode == FB_MODE_LINEAR)
        fb_fill_rect((int32_t)x, (int32_t)y, w, h, bg);
}

void fbcon_set_colors(fb_color_t fg, fb_color_t bg) {
    g_con.fg = fg;
    g_con.bg = bg;
}

void fbcon_clear(void) {
    if (g_fb.mode == FB_MODE_LINEAR)
        fb_fill_rect((int32_t)g_con.ox, (int32_t)g_con.oy,
                     g_con.ow, g_con.oh, g_con.bg);
    g_con.cur_col = 0;
    g_con.cur_row = 0;
}

static void fbcon_scroll(void) {
    if (g_fb.mode != FB_MODE_LINEAR) return;
    fb_blit((int32_t)g_con.ox,
            (int32_t)g_con.oy,
            (int32_t)g_con.ox,
            (int32_t)(g_con.oy + FB_FONT_H),
            g_con.ow,
            g_con.oh - (uint32_t)FB_FONT_H);
    fb_fill_rect((int32_t)g_con.ox,
                 (int32_t)(g_con.oy + g_con.oh - (uint32_t)FB_FONT_H),
                 g_con.ow, (uint32_t)FB_FONT_H, g_con.bg);
}

void fbcon_putc(char c) {
    if (g_fb.mode == FB_MODE_VGA) {
        dbg_putc(c);
        return;
    }

    if (c == '\r') { g_con.cur_col = 0; return; }

    if (c == '\n') {
        g_con.cur_col = 0;
        g_con.cur_row++;
        if (g_con.cur_row >= g_con.rows) {
            fbcon_scroll();
            g_con.cur_row = g_con.rows - 1;
        }
        return;
    }

    if (c == '\b') {
        if (g_con.cur_col > 0) {
            g_con.cur_col--;
            fb_fill_rect(
                (int32_t)(g_con.ox + g_con.cur_col * FB_FONT_W),
                (int32_t)(g_con.oy + g_con.cur_row * FB_FONT_H),
                FB_FONT_W, FB_FONT_H, g_con.bg);
        }
        return;
    }

    fb_draw_char(
        (int32_t)(g_con.ox + g_con.cur_col * FB_FONT_W),
        (int32_t)(g_con.oy + g_con.cur_row * FB_FONT_H),
        c, g_con.fg, g_con.bg);

    g_con.cur_col++;
    if (g_con.cur_col >= g_con.cols) {
        g_con.cur_col = 0;
        g_con.cur_row++;
        if (g_con.cur_row >= g_con.rows) {
            fbcon_scroll();
            g_con.cur_row = g_con.rows - 1;
        }
    }
}

void fbcon_puts(const char *str) {
    while (*str) fbcon_putc(*str++);
}

/* =========================================================================
 * UI — Рабочий стол FEXOS
 * ========================================================================= */

/* ---- CMOS RTC ---- */
static inline uint8_t cmos_read(uint8_t reg) {
    dbg_outb(0x70, reg);
    for (volatile int i = 0; i < 64; i++) (void)dbg_inb(0x80);
    return dbg_inb(0x71);
}

static inline int bcd_to_bin(uint8_t v) {
    return (v & 0x0F) + ((v >> 4) * 10);
}

static void rtc_get_msk(int *out_h, int *out_m, int *out_s) {
    /* Читаем ss/mm/hh ДВАЖДЫ и сравниваем. Если RTC начинает своё
     * внутреннее обновление ровно между отдельными inb() (проверка UIP
     * перед чтением не гарантирует атомарность всех трёх регистров разом),
     * можно получить "рваный" набор — например старое hh + новое mm.
     * На следующей секунде показания сами исправляются, что визуально
     * выглядит как скачок часов сразу на 2 секунды. Двойное чтение с
     * сверкой убирает эту гонку. */
    uint8_t ss, mm, hh, ss2, mm2, hh2;
    int guard;
    do {
        guard = 1000000;
        while ((cmos_read(0x0A) & 0x80) && guard-- > 0) ;
        ss = cmos_read(0x00);
        mm = cmos_read(0x02);
        hh = cmos_read(0x04);

        guard = 1000000;
        while ((cmos_read(0x0A) & 0x80) && guard-- > 0) ;
        ss2 = cmos_read(0x00);
        mm2 = cmos_read(0x02);
        hh2 = cmos_read(0x04);
    } while (ss != ss2 || mm != mm2 || hh != hh2);

    uint8_t sb = cmos_read(0x0B);

    if (!(sb & 0x04)) {
        ss = (uint8_t)bcd_to_bin(ss);
        mm = (uint8_t)bcd_to_bin(mm);
        hh = (uint8_t)bcd_to_bin(hh);
    }

    if (!(sb & 0x02)) {
        bool pm = (hh & 0x80) != 0;
        hh &= 0x7F;
        if (pm && hh != 12) hh += 12;
        else if (!pm && hh == 12) hh = 0;
    }

    int h = (int)hh + 3;
    if (h >= 24) h -= 24;

    *out_h = h;
    *out_m = (int)mm;
    *out_s = (int)ss;
}

static void fb_draw_digit2(int32_t x, int32_t y, int val,
                            fb_color_t fg, fb_color_t bg) {
    char buf[3];
    buf[0] = (char)('0' + (val / 10) % 10);
    buf[1] = (char)('0' + val % 10);
    buf[2] = '\0';
    fb_draw_string(x, y, buf, fg, bg);
}

#define TASKBAR_H       48u
#define TASKBAR_BG      0xD4D0C8U
#define MENU_BTN_X      0   /* вплотную к левому краю экрана — без зазора */
#define MENU_BTN_W      76u     /* было 64 — чуть шире */
#define MENU_BTN_H      40u     /* было 36 — чуть выше */
#define MENU_BTN_BG     0xFFFAA0U
#define MENU_BTN_FG     0x202020U
#define WALLPAPER_COLOR 0x6C9FCEU
#define CLOCK_FG        0x101010U
#define CLOCK_BG        TASKBAR_BG

/* =========================================================================
 * Всплывающее меню "Menu" (Пуск) — Shutdown / Reboot
 *
 * Прикреплено вплотную к taskbar (без зазора) и к левому краю кнопки
 * Menu. Панель светло-жёлтая (в цвет кнопки Menu), внутри две растянутые
 * по ширине светлые кнопки; при наведении курсора кнопка под ним
 * подсвечивается жёлтым.
 *
 * ВАЖНО: встроенный шрифт (g_font8x16) содержит только ASCII 0x20–0x7E,
 * кириллицы в нём нет, поэтому подписи — транслит.
 * ========================================================================= */
#define MENU_POPUP_W        190u
#define MENU_POPUP_H         98u
#define MENU_POPUP_GAP        0   /* 0 — панель "прилеплена" вплотную к taskbar */
#define MENU_POPUP_BG        0xFFFAA0U   /* светло-жёлтый, как кнопка Menu */
#define MENU_POPUP_BORDER    0xC8B860U   /* тёплая рамка в тон панели */
#define MENU_ITEM_W          (MENU_POPUP_W - 20u)
#define MENU_ITEM_H           36u
#define MENU_ITEM_GAP          8
#define MENU_ITEM_BG          0xFFFCE8U  /* светлая (почти белая) кнопка */
#define MENU_ITEM_BG_HOVER    0xFFE066U  /* жёлтая подсветка при наведении */
#define MENU_ITEM_FG          0x202020U

static bool       g_menu_open  = false;
static int        g_menu_hover = -1;      /* -1 нет, 0 = Shutdown, 1 = Reboot */
static uint8_t    g_prev_buttons = 0;
static fb_color_t *g_menu_saved_bg = NULL;

static void ui_system_shutdown(void);
static void ui_system_reboot(void);

/* =========================================================================
 * VFS
 * ========================================================================= */
extern int     vfs_open(const char *path, int flags);
extern int     vfs_close(int fd);
extern int64_t vfs_read(int fd, void *buf, uint64_t size);
extern int64_t vfs_seek(int fd, int64_t offset, int whence);

#ifndef UI_O_RDONLY
#  define UI_O_RDONLY 0
#endif

/* =========================================================================
 * ui_load_wallpaper — загружает UI/FexosLightW.png через png_decode.
 *
 * PNG должен быть 8-bit RGB или RGBA, non-interlaced.
 * Если файл не найден или декодирование провалилось — возвращает false,
 * и вызывающий код нальёт fallback-цвет (#6C9FCE).
 * ========================================================================= */
#define WALLPAPER_PNG_MAXSZ (16u * 1024u * 1024u)   /* 16 МБ */

static bool ui_load_wallpaper(uint32_t taskbar_y) {
    if (g_fb.mode != FB_MODE_LINEAR) return false;

    /* Открываем файл */
    int fd = vfs_open("UI/FexoxLightW.png", UI_O_RDONLY);
    if (fd < 0) fd = vfs_open("/UI/FexoxLightW.png", UI_O_RDONLY);
    if (fd < 0) {
        DBG_MSG("UI", "wallpaper PNG not found");
        return false;
    }

    /* Читаем весь файл в буфер */
    uint8_t *png_buf = (uint8_t *)kmalloc(WALLPAPER_PNG_MAXSZ);
    if (!png_buf) { vfs_close(fd); return false; }

    size_t total = 0;
    for (;;) {
        int64_t got = vfs_read(fd, png_buf + total,
                               (uint64_t)(WALLPAPER_PNG_MAXSZ - total));
        if (got <= 0) break;
        total += (size_t)got;
        if (total >= WALLPAPER_PNG_MAXSZ) break;
    }
    vfs_close(fd);

    if (total < 8) {
        DBG_MSG("UI", "wallpaper: file too small");
        kfree(png_buf);
        return false;
    }

    /* Декодируем PNG */
    png_image_t img;
    int rc = png_decode(png_buf, total, &img);
    kfree(png_buf);

    if (rc != PNG_OK) {
        DBG_VAL("UI", "wallpaper PNG decode failed rc", (uint64_t)(int64_t)rc);
        return false;
    }

    DBG_VAL("UI", "wallpaper w", img.width);
    DBG_VAL("UI", "wallpaper h", img.height);

    /* Масштабирование обоев под размер экрана (taskbar_y x g_fb.width).
     * Используем целочисленный билинейный метод (nearest-neighbour достаточно
     * для обоев и не требует умножения с плавающей точкой — freestanding).
     *
     * Для каждого пикселя экрана (dx, dy) вычисляем соответствующий пиксель
     * источника через fixed-point: src_x = dx * img.width / fb_width,
     * аналогично по Y. Это stretch-to-fill без сохранения пропорций.
     * Если нужно сохранить пропорции — добавить letterbox/pillarbox. */
    uint32_t dst_w = g_fb.width;
    uint32_t dst_h = taskbar_y;   /* область выше taskbar */

    for (uint32_t dy = 0; dy < dst_h; dy++) {
        /* src_y: маппинг dy -> строка в img (fixed-point, избегаем деления в цикле x) */
        uint32_t sy = (uint32_t)((uint64_t)dy * img.height / dst_h);
        if (sy >= img.height) sy = img.height - 1;

        uint8_t *dst_row = draw_buf() + (uint64_t)dy * g_fb.pitch;
        const uint8_t *src_row = img.pixels + (uint64_t)sy * img.width * 4u;

        if (g_fb.bytes_pp == 4) {
            uint32_t *dst32 = (uint32_t *)dst_row;
            for (uint32_t dx = 0; dx < dst_w; dx++) {
                uint32_t sx = (uint32_t)((uint64_t)dx * img.width / dst_w);
                if (sx >= img.width) sx = img.width - 1;
                const uint8_t *px = src_row + sx * 4u;
                dst32[dx] = rgb_to_native(fb_rgb(px[0], px[1], px[2]));
            }
        } else {
            for (uint32_t dx = 0; dx < dst_w; dx++) {
                uint32_t sx = (uint32_t)((uint64_t)dx * img.width / dst_w);
                if (sx >= img.width) sx = img.width - 1;
                const uint8_t *px = src_row + sx * 4u;
                uint32_t native = rgb_to_native(fb_rgb(px[0], px[1], px[2]));
                uint8_t *p = dst_row + (uint64_t)dx * g_fb.bytes_pp;
                switch (g_fb.bytes_pp) {
                    case 3: p[2] = (uint8_t)(native >> 16); /* fall */
                    case 2: p[1] = (uint8_t)(native >>  8); /* fall */
                    case 1: p[0] = (uint8_t)(native      ); break;
                }
            }
        }
    }

    kfree(img.pixels);
    return (dst_h > 0);
}

/* =========================================================================
 * ui_draw_taskbar_clock
 * ========================================================================= */
static void ui_draw_taskbar_clock(int32_t panel_y) {
    int hh, mm, ss;
    rtc_get_msk(&hh, &mm, &ss);

    int32_t cw = 8 * FB_FONT_W;
    int32_t cx = (int32_t)g_fb.width - cw - 8;
    int32_t cy = panel_y + (int32_t)((TASKBAR_H - FB_FONT_H) / 2);

    fb_draw_digit2(cx,               cy, hh, CLOCK_FG, CLOCK_BG);
    fb_draw_char  (cx + 16,          cy, ':', CLOCK_FG, CLOCK_BG);
    fb_draw_digit2(cx + 16 + 8,      cy, mm, CLOCK_FG, CLOCK_BG);
    fb_draw_char  (cx + 16 + 8 + 16, cy, ':', CLOCK_FG, CLOCK_BG);
    fb_draw_digit2(cx + 16 + 8 + 24, cy, ss, CLOCK_FG, CLOCK_BG);
}

void ui_redraw_clock(void) {
    if (g_fb.mode != FB_MODE_LINEAR) return;
    if (!g_fb.width || !g_fb.height) return;

    uint32_t panel_y = (g_fb.height >= TASKBAR_H) ? g_fb.height - TASKBAR_H : 0;
    int32_t cw = 8 * FB_FONT_W;
    int32_t cx = (int32_t)g_fb.width - cw - 8;

    fb_fill_rect(cx, (int32_t)panel_y, (uint32_t)cw + 8, TASKBAR_H, CLOCK_BG);
    ui_draw_taskbar_clock((int32_t)panel_y);
    fb_flip();
}

/* =========================================================================
 * ui_draw_menu_button
 * ========================================================================= */
static void ui_draw_menu_button(int32_t panel_y) {
    int32_t bx = MENU_BTN_X;
    int32_t by = panel_y + (int32_t)((TASKBAR_H - MENU_BTN_H) / 2);
    uint32_t bw = MENU_BTN_W;
    uint32_t bh = MENU_BTN_H;

    fb_fill_rect(bx, by, bw, bh, MENU_BTN_BG);

    fb_color_t hi  = 0xFFFFFFU;
    fb_color_t sh  = 0x808080U;
    fb_color_t sh2 = 0x404040U;

    fb_hline(bx,                         by,              bw, hi);
    fb_vline(bx,                         by,              bh, hi);
    fb_hline(bx,      (int32_t)(by + (int32_t)bh - 1),   bw, sh2);
    fb_vline((int32_t)(bx + (int32_t)bw - 1), by,         bh, sh2);

    fb_hline(bx + 1,                     by + 1,          bw - 2, 0xE0DCB0U);
    fb_vline(bx + 1,                     by + 1,          bh - 2, 0xE0DCB0U);
    fb_hline(bx + 1, (int32_t)(by + (int32_t)bh - 2),    bw - 2, sh);
    fb_vline((int32_t)(bx + (int32_t)bw - 2), by + 1,    bh - 2, sh);

    int32_t tx = bx + (int32_t)(bw / 2u) - (int32_t)(4u * FB_FONT_W / 2u);
    int32_t ty = by + (int32_t)((bh - FB_FONT_H) / 2u);
    fb_draw_string(tx, ty, "Menu", MENU_BTN_FG, MENU_BTN_BG);
}

/* =========================================================================
 * Всплывающее меню — геометрия, сохранение фона, отрисовка, клики
 * ========================================================================= */
static void menu_popup_rect(int32_t panel_y, int32_t *mx, int32_t *my) {
    *mx = MENU_BTN_X;
    *my = panel_y - (int32_t)MENU_POPUP_H - MENU_POPUP_GAP;
    if (*my < 0) *my = 0;
}

static void menu_item_rect(int idx, int32_t mx, int32_t my,
                            int32_t *ix, int32_t *iy, uint32_t *iw, uint32_t *ih) {
    *ix = mx + 10;
    *iy = my + 10 + idx * (int32_t)(MENU_ITEM_H + MENU_ITEM_GAP);
    *iw = MENU_ITEM_W;
    *ih = MENU_ITEM_H;
}

static bool point_in_rect(int32_t px, int32_t py,
                           int32_t x, int32_t y, uint32_t w, uint32_t h) {
    return px >= x && py >= y && px < x + (int32_t)w && py < y + (int32_t)h;
}

static void menu_save_bg(int32_t mx, int32_t my) {
    if (!g_menu_saved_bg) {
        g_menu_saved_bg = (fb_color_t *)kmalloc(
            (uint64_t)MENU_POPUP_W * MENU_POPUP_H * sizeof(fb_color_t));
    }
    if (!g_menu_saved_bg) return;
    for (uint32_t row = 0; row < MENU_POPUP_H; row++)
        for (uint32_t col = 0; col < MENU_POPUP_W; col++)
            g_menu_saved_bg[row * MENU_POPUP_W + col] =
                fb_get_pixel(mx + (int32_t)col, my + (int32_t)row);
}

static void menu_restore_bg(int32_t mx, int32_t my) {
    if (!g_menu_saved_bg) return;
    for (uint32_t row = 0; row < MENU_POPUP_H; row++)
        for (uint32_t col = 0; col < MENU_POPUP_W; col++)
            fb_put_pixel(mx + (int32_t)col, my + (int32_t)row,
                        g_menu_saved_bg[row * MENU_POPUP_W + col]);
    fb_flip();
}

static void ui_draw_start_menu(int32_t panel_y) {
    int32_t mx, my;
    menu_popup_rect(panel_y, &mx, &my);

    /* Панель: белая с лёгким серо-жёлтым оттенком + серая рамка */
    fb_fill_rect(mx, my, MENU_POPUP_W, MENU_POPUP_H, MENU_POPUP_BG);
    fb_draw_rect(mx, my, MENU_POPUP_W, MENU_POPUP_H, MENU_POPUP_BORDER, 1);

    static const char *labels[2] = { "Shutdown", "Reboot" };

    for (int i = 0; i < 2; i++) {
        int32_t ix, iy; uint32_t iw, ih;
        menu_item_rect(i, mx, my, &ix, &iy, &iw, &ih);

        fb_color_t bg = (g_menu_hover == i) ? MENU_ITEM_BG_HOVER : MENU_ITEM_BG;
        fb_fill_rect(ix, iy, iw, ih, bg);
        fb_draw_rect(ix, iy, iw, ih, MENU_POPUP_BORDER, 1);

        int32_t tw = (int32_t)fb_text_width(labels[i]);
        int32_t tx = ix + (int32_t)((iw - (uint32_t)tw) / 2u);
        int32_t ty = iy + (int32_t)((ih - FB_FONT_H) / 2u);
        fb_draw_string(tx, ty, labels[i], MENU_ITEM_FG, bg);
    }

    fb_flip();
}

/*
 * ui_desktop_handle_mouse — вызывать на каждом тике (из ui_tick, ДО
 * перерисовки курсора) с текущей позицией мыши и состоянием кнопок.
 * Открывает/закрывает Menu-меню по клику на кнопку Menu, подсвечивает
 * пункты меню под курсором и обрабатывает клики по "Shutdown"/"Reboot".
 */
void ui_desktop_handle_mouse(int32_t x, int32_t y, uint8_t buttons) {
    if (g_fb.mode != FB_MODE_LINEAR) return;
    if (!g_fb.width || !g_fb.height) return;

    int32_t panel_y = (int32_t)((g_fb.height >= TASKBAR_H) ? g_fb.height - TASKBAR_H : 0);

    bool left_now  = (buttons & 0x01) != 0;
    bool left_was  = (g_prev_buttons & 0x01) != 0;
    bool left_click = left_now && !left_was;   /* фронт нажатия */

    /* Клик по кнопке Menu — открыть/закрыть меню */
    int32_t mbx = MENU_BTN_X;
    int32_t mby = panel_y + (int32_t)((TASKBAR_H - MENU_BTN_H) / 2);
    if (left_click && point_in_rect(x, y, mbx, mby, MENU_BTN_W, MENU_BTN_H)) {
        int32_t mx, my;
        menu_popup_rect(panel_y, &mx, &my);
        if (g_menu_open) {
            menu_restore_bg(mx, my);
            g_menu_open = false;
        } else {
            menu_save_bg(mx, my);
            g_menu_open = true;
            g_menu_hover = -1;
            ui_draw_start_menu(panel_y);
        }
        g_prev_buttons = buttons;
        return;
    }

    if (g_menu_open) {
        int32_t mx, my;
        menu_popup_rect(panel_y, &mx, &my);

        int new_hover = -1;
        for (int i = 0; i < 2; i++) {
            int32_t ix, iy; uint32_t iw, ih;
            menu_item_rect(i, mx, my, &ix, &iy, &iw, &ih);
            if (point_in_rect(x, y, ix, iy, iw, ih)) { new_hover = i; break; }
        }
        if (new_hover != g_menu_hover) {
            g_menu_hover = new_hover;
            ui_draw_start_menu(panel_y);
        }

        if (left_click && new_hover >= 0) {
            if (new_hover == 0) ui_system_shutdown();
            else                ui_system_reboot();
            /* не возвращаемся — на случай если shutdown/reboot не сработали
             * (реальное железо без ACPI/8042), меню остаётся открытым */
        } else if (left_click && new_hover < 0 &&
                   !point_in_rect(x, y, mx, my, MENU_POPUP_W, MENU_POPUP_H)) {
            /* клик мимо меню и мимо кнопки Menu — закрыть */
            menu_restore_bg(mx, my);
            g_menu_open = false;
        }
    }

    g_prev_buttons = buttons;
}

/*
 * ui_system_shutdown — выключение питания.
 *
 * Порядок попыток (каждая следующая — фолбэк, если предыдущая не сработала):
 *   1. Настоящий ACPI S5 (AcpiControl.c: RSDP->FADT->\_S5, запись
 *      SLP_TYP|SLP_EN в PM1_CNT) — работает и на реальном ПК, и в QEMU.
 *   2. QEMU/Bochs debug shutdown-порт (0x604/0xB004) — на реальном железе
 *      это просто запись в никуда, безвредно, но и бесполезно; оставлено
 *      на случай сборки без ACPI-таблиц (например урезанный QEMU-профиль).
 *   3. hlt-цикл — если вообще ничего не сработало, машина зависает,
 *      а не продолжает работать с открытым меню.
 */
static void ui_system_shutdown(void) {
    DBG_MSG("UI", "shutdown requested");

    acpi_power_off();   /* при успехе сюда управление не возвращается */

    DBG_MSG("UI", "shutdown: ACPI недоступен/не сработал, QEMU-фолбэк");
    __asm__ volatile ("outw %0, %1" : : "a"((uint16_t)0x2000), "Nd"((uint16_t)0x604));
    __asm__ volatile ("outw %0, %1" : : "a"((uint16_t)0x2000), "Nd"((uint16_t)0xB004));

    for (;;) __asm__ volatile ("hlt");
}

/*
 * ui_system_reboot — программная перезагрузка.
 *
 * Порядок попыток:
 *   1. ACPI Reset Register (FADT, ACPI 2.0+) — самый надёжный способ на
 *      современном реальном железе, официально документирован в ACPI spec.
 *   2. Импульс RESET через контроллер клавиатуры 8042 (команда 0xFE в
 *      порт 0x64) — классика, работает почти на всём железе с i8042
 *      (и в QEMU), но на некоторых чипсетах без i8042 (USB-only,
 *      "legacy-free") не срабатывает.
 *   3. Triple fault (lidt с limit=0 + программное исключение) — работает
 *      абсолютно всегда на x86, это последний гарантированный фолбэк.
 */
static void ui_system_reboot(void) {
    DBG_MSG("UI", "reboot requested");

    acpi_reset();        /* при успехе сюда управление не возвращается */

    DBG_MSG("UI", "reboot: ACPI reset недоступен, 8042-фолбэк");
    for (int i = 0; i < 100; i++) {
        uint8_t status;
        __asm__ volatile ("inb %1, %0" : "=a"(status) : "Nd"((uint16_t)0x64));
        if (!(status & 0x02)) break;
    }
    __asm__ volatile ("outb %0, %1" : : "a"((uint8_t)0xFE), "Nd"((uint16_t)0x64));

    /* Даём 8042 немного времени сработать, прежде чем идти на triple fault */
    for (volatile uint32_t i = 0; i < 10000000u; i++) { }

    DBG_MSG("UI", "reboot: 8042 не сработал, triple fault");
    cpu_triple_fault_reset();   /* никогда не возвращается */
}

/* =========================================================================
 * ui_draw_desktop
 * ========================================================================= */
void ui_draw_desktop(void) {
    if (g_fb.mode != FB_MODE_LINEAR) return;

    uint32_t w = g_fb.width;
    uint32_t h = g_fb.height;
    if (!w || !h) return;

    uint32_t panel_y = (h >= TASKBAR_H) ? h - TASKBAR_H : 0;

    DBG_MSG("UI", "draw_desktop: wallpaper...");

    bool wallpaper_ok = false;
    if (panel_y > 0) {
        wallpaper_ok = ui_load_wallpaper(panel_y);
    }
    if (!wallpaper_ok) {
        fb_fill_rect(0, 0, w, panel_y, WALLPAPER_COLOR);
        DBG_MSG("UI", "wallpaper fallback color");
    }

    fb_fill_rect(0, (int32_t)panel_y, w, TASKBAR_H, TASKBAR_BG);
    ui_draw_menu_button((int32_t)panel_y);
    ui_draw_taskbar_clock((int32_t)panel_y);

    fb_flip();

    DBG_MSG("UI", "draw_desktop: done");
}
