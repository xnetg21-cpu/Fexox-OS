/*
 * Framebuffer.h — драйвер экрана для FEXOS
 *
 * Поддерживает два режима:
 *   FB_MODE_LINEAR  — GOP linear framebuffer (UEFI, 32 bpp)
 *   FB_MODE_VGA     — VGA text mode 80×25 (fallback, без пикселей)
 *
 * В linear-режиме доступны:
 *   - рисование пикселей, прямоугольников, линий
 *   - вывод текста (встроенный шрифт 8×16, ASCII 0x20–0x7E)
 *   - double-buffering: рисовать в back-buffer, затем fb_flip()
 *   - цветовые константы (RGB24)
 *
 * Информация о framebuffer передаётся загрузчиком через BOOT_INFO
 * (поля fb_* добавляются в структуру).
 *
 * Использование:
 *   1. Добавить поля GOP в BOOT_INFO (см. ниже fb_info_t).
 *   2. В kernel_entry: fb_init(&info->fb); после mem_control_init.
 *   3. Рисовать через fb_fill_rect, fb_draw_text и т.п.
 *   4. Вызывать fb_flip() чтобы показать результат.
 */

#ifndef FRAMEBUFFER_H
#define FRAMEBUFFER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* =========================================================================
 * Режимы работы
 * ========================================================================= */
#define FB_MODE_NONE    0   /* не инициализирован */
#define FB_MODE_VGA     1   /* VGA text 80×25 */
#define FB_MODE_LINEAR  2   /* GOP linear framebuffer */

/* =========================================================================
 * fb_info_t — описание framebuffer от загрузчика
 *
 * Заполняется UEFI-загрузчиком и кладётся в BOOT_INFO.
 * Если GOP недоступен — mode=FB_MODE_VGA, остальные поля = 0.
 * ========================================================================= */
typedef struct {
    uint8_t  mode;          /* FB_MODE_LINEAR или FB_MODE_VGA              */
    uint32_t width;         /* ширина в пикселях                           */
    uint32_t height;        /* высота в пикселях                           */
    uint32_t pitch;         /* байт на строку (stride)                     */
    uint8_t  bpp;           /* бит на пиксель (32 = RGBX/BGRX)            */
    uint64_t phys_addr;     /* физический адрес буфера                     */
    /* Маски каналов (из EFI_GRAPHICS_OUTPUT_MODE_INFORMATION):            */
    uint32_t red_mask;      /* например 0x00FF0000 для BGRX                */
    uint32_t green_mask;    /* например 0x0000FF00                         */
    uint32_t blue_mask;     /* например 0x000000FF                         */
} fb_info_t;

/* =========================================================================
 * Цвета (RGB24 — независимо от порядка каналов на железе)
 *
 * Функции принимают fb_color_t и сами переставляют каналы
 * согласно маскам устройства.
 * ========================================================================= */
typedef uint32_t fb_color_t;   /* 0x00RRGGBB */

#define FB_BLACK        0x000000U
#define FB_WHITE        0xFFFFFFU
#define FB_RED          0xFF0000U
#define FB_GREEN        0x00FF00U
#define FB_BLUE         0x0000FFU
#define FB_CYAN         0x00FFFFU
#define FB_MAGENTA      0xFF00FFU
#define FB_YELLOW       0xFFFF00U
#define FB_GRAY         0x808080U
#define FB_DARK_GRAY    0x404040U
#define FB_LIGHT_GRAY   0xC0C0C0U
#define FB_ORANGE       0xFF8000U
#define FB_DARK_BLUE    0x000080U
#define FB_DARK_GREEN   0x008000U

/* Конструктор цвета из компонент */
static inline fb_color_t fb_rgb(uint8_t r, uint8_t g, uint8_t b) {
    return ((fb_color_t)r << 16) | ((fb_color_t)g << 8) | b;
}

/* =========================================================================
 * Прямоугольник (используется во многих функциях)
 * ========================================================================= */
typedef struct {
    int32_t x, y;
    uint32_t w, h;
} fb_rect_t;

/* =========================================================================
 * Инициализация
 * ========================================================================= */

/*
 * fb_init — инициализировать драйвер.
 *   info   — указатель на fb_info_t из BOOT_INFO.
 *            Если NULL или mode=VGA — переходит в VGA-режим.
 *
 * Вызывать ПОСЛЕ mem_control_init (нужен DIRECT_MAP для маппинга буфера).
 * Возвращает FB_MODE_*.
 */
int fb_init(const fb_info_t *info);

/* Текущий режим */
int fb_get_mode(void);

/* Ширина и высота в пикселях (0 в VGA-режиме) */
uint32_t fb_width(void);
uint32_t fb_height(void);

/* =========================================================================
 * Примитивы рисования (только FB_MODE_LINEAR)
 * ========================================================================= */

/* Нарисовать один пиксель */
void fb_put_pixel(int32_t x, int32_t y, fb_color_t color);

/* Прочитать пиксель (RGB24, 0x00RRGGBB). Нужен для сохранения фона под курсором. */
fb_color_t fb_get_pixel(int32_t x, int32_t y);


/* Залить прямоугольник */
void fb_fill_rect(int32_t x, int32_t y, uint32_t w, uint32_t h,
                  fb_color_t color);

/* Обводка прямоугольника */
void fb_draw_rect(int32_t x, int32_t y, uint32_t w, uint32_t h,
                  fb_color_t color, uint32_t thickness);

/* Горизонтальная линия (быстрая) */
void fb_hline(int32_t x, int32_t y, uint32_t len, fb_color_t color);

/* Вертикальная линия (быстрая) */
void fb_vline(int32_t x, int32_t y, uint32_t len, fb_color_t color);

/* Линия Брезенхема (любой наклон) */
void fb_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1,
             fb_color_t color);

/* Залить весь экран */
void fb_clear(fb_color_t color);

/* Скопировать прямоугольник внутри буфера */
void fb_blit(int32_t dst_x, int32_t dst_y,
             int32_t src_x, int32_t src_y,
             uint32_t w, uint32_t h);

/* =========================================================================
 * Текст (встроенный шрифт 8×16, ASCII 0x20–0x7E)
 * ========================================================================= */

/*
 * fb_draw_char — нарисовать один символ.
 *   x, y     — верхний левый угол символа
 *   c        — символ (< 0x20 или > 0x7E → отображается как '?')
 *   fg, bg   — цвета; если bg == FB_TRANSPARENT_BG — фон прозрачный
 */
#define FB_TRANSPARENT_BG 0xFF000000U   /* специальное значение: прозрачный фон */

void fb_draw_char(int32_t x, int32_t y, char c,
                  fb_color_t fg, fb_color_t bg);

/*
 * fb_draw_string — нарисовать строку начиная с (x, y).
 *   Символы \n и \r не обрабатываются; только горизонтальный вывод.
 *   Возвращает x после последнего символа.
 */
int32_t fb_draw_string(int32_t x, int32_t y, const char *str,
                       fb_color_t fg, fb_color_t bg);

/* Ширина символа и строки в пикселях */
#define FB_FONT_W   8
#define FB_FONT_H   16

static inline uint32_t fb_text_width(const char *str) {
    uint32_t n = 0;
    while (*str++) n++;
    return n * FB_FONT_W;
}

/* =========================================================================
 * Текстовый терминал (fb_console)
 *
 * Поверх fb_draw_char — хранит курсор, делает перенос строки и скролл.
 * Удобен для вывода ядра вместо VGA kprint.
 * ========================================================================= */

/* Инициализация консоли (вызывать после fb_init) */
void fbcon_init(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                fb_color_t fg, fb_color_t bg);

/* Вывести строку (поддерживает \n) */
void fbcon_puts(const char *str);

/* Вывести символ */
void fbcon_putc(char c);

/* Очистить область консоли */
void fbcon_clear(void);

/* Установить цвета */
void fbcon_set_colors(fb_color_t fg, fb_color_t bg);

/* =========================================================================
 * Double-buffering
 *
 * По умолчанию рисование идёт прямо в физический буфер (fb_front).
 * Если включить double-buffering — все примитивы рисуют в back-buffer
 * (в RAM), fb_flip() копирует его на экран memcpy-ом.
 * Это убирает мерцание при сложной отрисовке.
 * ========================================================================= */

/* Включить double-buffering (выделяет буфер через kmalloc).
   Возвращает 0 при успехе. */
int fb_enable_double_buffer(void);

/* Отключить и освободить back-buffer */
void fb_disable_double_buffer(void);

/* Скопировать back-buffer → экран */
void fb_flip(void);

/* Прямой доступ к back-buffer (для пользовательского blit) */
void *fb_back_ptr(void);

/* =========================================================================
 * Вспомогательные макросы
 * ========================================================================= */

/* Рисовать только если координаты внутри экрана */
#define FB_IN_BOUNDS(x, y) \
    ((uint32_t)(x) < fb_width() && (uint32_t)(y) < fb_height())


/* =========================================================================
 * UI рабочий стол
 * ========================================================================= */

/*
 * ui_draw_desktop — рисует рабочий стол:
 *   - обои из UI/FexosLightW.raw (32bpp BGRA raw, w*h*4 байт)
 *     или цветной fallback (#6C9FCE) если файл не найден
 *   - taskbar снизу (~48px, Win98-серый #D4D0C8)
 *   - кнопка "Menu" слева (светло-жёлтая, Win98 3D-стиль)
 *   - часы справа (24ч, МСК = UTC+3, из CMOS RTC)
 *
 * Вызывать после fb_init() и монтирования VFS.
 * В FB_MODE_VGA — ничего не делает.
 */
void ui_draw_desktop(void);

/* Перерисовать только область часов на taskbar (для "тикающих" часов).
 * Вызывать раз в секунду из таймерного тика. См. ui_extra.h/ui_tick(). */
void ui_redraw_clock(void);

#endif /* FRAMEBUFFER_H */
