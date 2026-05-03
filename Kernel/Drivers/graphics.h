#ifndef GRAPHICS_H
#define GRAPHICS_H

#include <stdint.h>

#define VGA_COLOR_BLACK         0x0
#define VGA_COLOR_BLUE          0x1
#define VGA_COLOR_GREEN         0x2
#define VGA_COLOR_CYAN          0x3
#define VGA_COLOR_RED           0x4
#define VGA_COLOR_MAGENTA       0x5
#define VGA_COLOR_BROWN         0x6
#define VGA_COLOR_LIGHT_GRAY    0x7
#define VGA_COLOR_DARK_GRAY     0x8
#define VGA_COLOR_LIGHT_BLUE    0x9
#define VGA_COLOR_LIGHT_GREEN   0xA
#define VGA_COLOR_LIGHT_CYAN    0xB
#define VGA_COLOR_LIGHT_RED     0xC
#define VGA_COLOR_LIGHT_MAGENTA 0xD
#define VGA_COLOR_YELLOW        0xE
#define VGA_COLOR_WHITE         0xF

void graphics_init(void);
void graphics_clear(uint8_t color);
void graphics_print(const char *text);
void graphics_print_at(uint32_t row, uint32_t col, const char *text, uint8_t color);
void graphics_show_banner(const char *title);
void graphics_show_status(const char *text);
void graphics_print_error(const char *text);

#endif // GRAPHICS_H
