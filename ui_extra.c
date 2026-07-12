/*
 * ui_extra.c — курсор из PNG (с альфа-каналом) + "тикающие" часы
 *
 * Зависимости: Framebuffer (fb_get_pixel/fb_put_pixel/fb_flip/ui_redraw_clock),
 * png.c (декодер PNG), VFS (vfs_open/vfs_read/vfs_close), klibc (kmalloc/kfree).
 */
#include <stdint.h>
#include <stddef.h>
#include "Framebuffer.h"
#include "png.h"
#include "ui_extra.h"
#include "debug_out.h"

extern void *kmalloc(uint64_t size);
extern void  kfree(void *ptr);

extern int     vfs_open(const char *path, int flags);
extern int     vfs_close(int fd);
extern int64_t vfs_read(int fd, void *buf, uint64_t size);

/* --- из PS2Mouse.c --- */
extern uint8_t ps2_mouse_buttons(void);

#ifndef UI_O_RDONLY
#define UI_O_RDONLY 0
#endif

/* =========================================================================
 * Частота таймерного тика (см. interrupt_init(..., timer_freq_hz) —
 * по умолчанию 100 Гц). Используется только для определения "раз в секунду".
 * ========================================================================= */
#define UI_TICK_HZ          100

/* Ограничение размера курсора — стандартные размеры 16x16/24x24/32x32 */
#define CURSOR_MAX_W        32
#define CURSOR_MAX_H        32

/* Максимальный размер файла cursor.png, который мы готовы прочитать.
 * 256 КБ — достаточно для любого разумного курсора (32x32 RGBA PNG). */
#define CURSOR_FILE_MAXSZ   (256 * 1024)

/* =========================================================================
 * Состояние
 * ========================================================================= */
static png_image_t g_cursor;         /* g_cursor.pixels == NULL -> fallback */
static int         g_cursor_loaded = 0;

static int32_t g_mouse_x = 100;
static int32_t g_mouse_y = 100;

/* Сохранённый фон под курсором (для восстановления перед перерисовкой) */
static fb_color_t g_saved_bg[CURSOR_MAX_W * CURSOR_MAX_H];
static int32_t    g_saved_x = -1, g_saved_y = -1;
static uint32_t   g_saved_w = 0,  g_saved_h = 0;
static int        g_saved_valid = 0;

static uint32_t   g_tick_counter = 0;

/* =========================================================================
 * Загрузка cursor.png через VFS
 * ========================================================================= */
static int load_cursor_png(void) {
    /* Пробуем оба варианта пути: с ведущим слешем и без */
    DBG_MSG("UI", "cursor: trying UI/cursor.png");
    int fd = vfs_open("UI/cursor.png", UI_O_RDONLY);
    if (fd < 0) {
        DBG_MSG("UI", "cursor: trying /UI/cursor.png");
        fd = vfs_open("/UI/cursor.png", UI_O_RDONLY);
    }
    if (fd < 0) {
        DBG_MSG("UI", "cursor.png not found, using fallback arrow");
        return -1;
    }
    DBG_VAL("UI", "cursor: fd", (uint64_t)fd);

    uint8_t *buf = (uint8_t *)kmalloc(CURSOR_FILE_MAXSZ);
    if (!buf) {
        DBG_MSG("UI", "cursor: kmalloc failed");
        vfs_close(fd);
        return -1;
    }

    size_t total = 0;
    for (;;) {
        int64_t got = vfs_read(fd, buf + total, CURSOR_FILE_MAXSZ - total);
        if (got <= 0) break;
        total += (size_t)got;
        if (total >= CURSOR_FILE_MAXSZ) break;
    }
    vfs_close(fd);

    DBG_VAL("UI", "cursor: bytes read", (uint64_t)total);

    if (total == 0) {
        DBG_MSG("UI", "cursor: empty file");
        kfree(buf);
        return -1;
    }

    /* Быстрая проверка PNG-сигнатуры перед декодированием */
    if (total < 8 || buf[0] != 0x89 || buf[1] != 'P' ||
        buf[2] != 'N' || buf[3] != 'G') {
        DBG_MSG("UI", "cursor: not a PNG (bad magic)");
        kfree(buf);
        return -1;
    }

    int rc = png_decode(buf, total, &g_cursor);
    kfree(buf);

    if (rc != PNG_OK) {
        DBG_VAL("UI", "cursor.png decode failed, rc", (uint64_t)(int64_t)rc);
        return -1;
    }
    if (g_cursor.width > CURSOR_MAX_W || g_cursor.height > CURSOR_MAX_H) {
        DBG_VAL("UI", "cursor.png too large, w", g_cursor.width);
        DBG_VAL("UI", "cursor.png too large, h", g_cursor.height);
        DBG_MSG("UI", "cursor.png max is 32x32, using fallback arrow");
        kfree(g_cursor.pixels);
        g_cursor.pixels = NULL;
        return -1;
    }

    DBG_VAL("UI", "cursor.png loaded OK, w", g_cursor.width);
    DBG_VAL("UI", "cursor.png loaded OK, h", g_cursor.height);
    return 0;
}

int ui_extra_init(void) {
    g_cursor.pixels = NULL;
    if (load_cursor_png() == 0) g_cursor_loaded = 1;
    /* Если PNG не загружен — будет использоваться встроенная стрелка
     * (см. draw_fallback_arrow ниже). ui_extra_init всегда "успешен",
     * чтобы вызывающий код не падал из-за отсутствия файла. */
    return 0;
}

void ui_set_mouse_pos(int32_t x, int32_t y) {
    g_mouse_x = x;
    g_mouse_y = y;
}

/* =========================================================================
 * Встроенная стрелка-курсор (если cursor.png отсутствует/не декодируется).
 * 12x19, классическая Win-стрелка: '#' = чёрный контур, '.' = белая заливка,
 * пробел = прозрачный.
 * ========================================================================= */
static const char g_fallback_arrow[19][12] = {
    "#           ",
    "##          ",
    "#.#         ",
    "#..#        ",
    "#...#       ",
    "#....#      ",
    "#.....#     ",
    "#......#    ",
    "#.......#   ",
    "#........#  ",
    "#.....##### ",
    "#...#       ",
    "#..##       ",
    "#.#.#       ",
    "#. #.#      ",
    "#   #.#     ",
    "    #.#     ",
    "     #      ",
    "            ",
};
#define FALLBACK_W 12
#define FALLBACK_H 19

/* =========================================================================
 * Сохранение/восстановление фона под курсором
 * ========================================================================= */
static void cursor_save_bg(int32_t x, int32_t y, uint32_t w, uint32_t h) {
    for (uint32_t row = 0; row < h; row++) {
        for (uint32_t col = 0; col < w; col++) {
            g_saved_bg[row * w + col] = fb_get_pixel(x + (int32_t)col, y + (int32_t)row);
        }
    }
    g_saved_x = x; g_saved_y = y;
    g_saved_w = w; g_saved_h = h;
    g_saved_valid = 1;
}

static void cursor_restore_bg(void) {
    if (!g_saved_valid) return;
    for (uint32_t row = 0; row < g_saved_h; row++) {
        for (uint32_t col = 0; col < g_saved_w; col++) {
            fb_put_pixel(g_saved_x + (int32_t)col, g_saved_y + (int32_t)row,
                         g_saved_bg[row * g_saved_w + col]);
        }
    }
    g_saved_valid = 0;
}

/* Простое альфа-смешение: result = src*a/255 + dst*(255-a)/255 */
static inline uint8_t blend_chan(uint8_t src, uint8_t dst, uint8_t a) {
    return (uint8_t)(((uint32_t)src * a + (uint32_t)dst * (255 - a)) / 255);
}

/* =========================================================================
 * Отрисовка курсора в позиции (x, y) — top-left угол изображения.
 * Сначала сохраняет фон под новой позицией, затем рисует курсор
 * с альфа-блендингом (PNG) или встроенную стрелку (fallback).
 * ========================================================================= */
static void cursor_draw(int32_t x, int32_t y) {
    if (g_cursor_loaded && g_cursor.pixels) {
        uint32_t w = g_cursor.width, h = g_cursor.height;
        cursor_save_bg(x, y, w, h);

        for (uint32_t row = 0; row < h; row++) {
            for (uint32_t col = 0; col < w; col++) {
                const uint8_t *px = g_cursor.pixels + (((size_t)row * w + col) * 4);
                uint8_t r = px[0], g = px[1], b = px[2], a = px[3];
                if (a == 0) continue; /* полностью прозрачный — оставляем фон */

                int32_t sx = x + (int32_t)col;
                int32_t sy = y + (int32_t)row;

                if (a == 255) {
                    fb_put_pixel(sx, sy, fb_rgb(r, g, b));
                } else {
                    fb_color_t bg = fb_get_pixel(sx, sy);
                    uint8_t br = (uint8_t)((bg >> 16) & 0xFF);
                    uint8_t bgc = (uint8_t)((bg >> 8) & 0xFF);
                    uint8_t bb = (uint8_t)(bg & 0xFF);
                    fb_color_t mixed = fb_rgb(blend_chan(r, br, a),
                                               blend_chan(g, bgc, a),
                                               blend_chan(b, bb, a));
                    fb_put_pixel(sx, sy, mixed);
                }
            }
        }
    } else {
        /* Fallback: встроенная стрелка из примитивов, без блендинга */
        cursor_save_bg(x, y, FALLBACK_W, FALLBACK_H);
        for (int row = 0; row < FALLBACK_H; row++) {
            for (int col = 0; col < FALLBACK_W; col++) {
                char c = g_fallback_arrow[row][col];
                if (c == '#') fb_put_pixel(x + col, y + row, FB_BLACK);
                else if (c == '.') fb_put_pixel(x + col, y + row, FB_WHITE);
                /* пробел — оставляем фон как есть */
            }
        }
    }
}

/* =========================================================================
 * ui_tick — вызывать из таймерного прерывания (sched_tick).
 *
 *   - каждый тик: восстановить фон под старым курсором, нарисовать курсор
 *     в новой позиции (g_mouse_x/g_mouse_y, обновляется ui_set_mouse_pos)
 *   - раз в секунду (UI_TICK_HZ тиков): перерисовать часы
 * ========================================================================= */
void ui_tick(void) {
    if (fb_get_mode() != FB_MODE_LINEAR) return;

    /* ВАЖНО: сначала стираем курсор с его СТАРОЙ позиции (возвращаем
     * реальный фон рабочего стола), и только ПОТОМ трогаем
     * Menu/иконки/окна fxapp. Раньше было наоборот, и это давало
     * характерный баг: пока курсор ещё не стёрт, его спрайт физически
     * лежит поверх кадра — и когда окно fxapp в этот момент
     * сохраняет/восстанавливает свой фон (при перетаскивании/закрытии),
     * оно захватывает пиксели курсора как "фон". Позже cursor_restore_bg
     * накладывал уже устаревший сохранённый кусок обратно поверх
     * свежеперерисованного окна — и на столе оставались "хвосты"
     * (кусочки синего заголовка/красной кнопки close). Стирая курсор
     * ДО перерисовки окна, мы гарантируем, что сохранение/восстановление
     * фона окна всегда работает с чистым содержимым рабочего стола. */
    cursor_restore_bg();

    ui_desktop_handle_mouse(g_mouse_x, g_mouse_y, ps2_mouse_buttons());

    cursor_draw(g_mouse_x, g_mouse_y);

    g_tick_counter++;
    if (g_tick_counter >= UI_TICK_HZ) {
        g_tick_counter = 0;
        ui_redraw_clock();
    }

    fb_flip();
}
