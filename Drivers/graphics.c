#include "graphics.h"

#define VGA_WIDTH 80
#define VGA_HEIGHT 25
#define VGA_MEMORY_ADDRESS 0xB8000

static volatile uint16_t *const VGA_BUFFER = (volatile uint16_t *)VGA_MEMORY_ADDRESS;
static uint32_t cursor_row = 0;
static uint32_t cursor_col = 0;
static uint8_t default_color = VGA_COLOR_LIGHT_GRAY | (VGA_COLOR_BLUE << 4);

static inline uint8_t graphics_make_color(uint8_t fg, uint8_t bg) {
    return fg | (bg << 4);
}

static inline uint16_t graphics_entry(char c, uint8_t color) {
    return (uint16_t)c | ((uint16_t)color << 8);
}

static uint32_t graphics_strlen(const char *str) {
    uint32_t len = 0;
    while (str && str[len]) {
        len++;
    }
    return len;
}

static void graphics_copy_region(uint32_t dest_row, uint32_t src_row, uint32_t row_count) {
    for (uint32_t row = 0; row < row_count; row++) {
        for (uint32_t col = 0; col < VGA_WIDTH; col++) {
            VGA_BUFFER[(dest_row + row) * VGA_WIDTH + col] = VGA_BUFFER[(src_row + row) * VGA_WIDTH + col];
        }
    }
}

static void graphics_clear_row(uint32_t row, uint8_t color) {
    uint16_t entry = graphics_entry(' ', color);
    for (uint32_t col = 0; col < VGA_WIDTH; col++) {
        VGA_BUFFER[row * VGA_WIDTH + col] = entry;
    }
}

static void graphics_scroll_if_needed(void) {
    if (cursor_row < VGA_HEIGHT) {
        return;
    }
    graphics_copy_region(0, 1, VGA_HEIGHT - 1);
    graphics_clear_row(VGA_HEIGHT - 1, default_color);
    cursor_row = VGA_HEIGHT - 1;
}

void graphics_clear(uint8_t color) {
    uint16_t entry = graphics_entry(' ', color);
    for (uint32_t row = 0; row < VGA_HEIGHT; row++) {
        for (uint32_t col = 0; col < VGA_WIDTH; col++) {
            VGA_BUFFER[row * VGA_WIDTH + col] = entry;
        }
    }
    cursor_row = 0;
    cursor_col = 0;
}

void graphics_init(void) {
    default_color = graphics_make_color(VGA_COLOR_WHITE, VGA_COLOR_BLUE);
    graphics_clear(default_color);
}

void graphics_print_at(uint32_t row, uint32_t col, const char *text, uint8_t color) {
    if (!text || row >= VGA_HEIGHT || col >= VGA_WIDTH) {
        return;
    }
    uint32_t x = col;
    uint32_t y = row;
    uint16_t entry;

    while (*text && y < VGA_HEIGHT) {
        if (*text == '\n' || x >= VGA_WIDTH) {
            x = col;
            y++;
            if (*text == '\n') {
                text++;
                continue;
            }
            if (y >= VGA_HEIGHT) {
                break;
            }
        }
        entry = graphics_entry(*text, color);
        VGA_BUFFER[y * VGA_WIDTH + x] = entry;
        x++;
        text++;
    }
}

void graphics_print(const char *text) {
    if (!text) {
        return;
    }

    while (*text) {
        char c = *text++;
        if (c == '\n') {
            cursor_col = 0;
            cursor_row++;
            graphics_scroll_if_needed();
            continue;
        }

        VGA_BUFFER[cursor_row * VGA_WIDTH + cursor_col] = graphics_entry(c, default_color);
        cursor_col++;

        if (cursor_col >= VGA_WIDTH) {
            cursor_col = 0;
            cursor_row++;
        }

        if (cursor_row >= VGA_HEIGHT) {
            graphics_scroll_if_needed();
        }
    }
}

void graphics_show_banner(const char *title) {
    const char *banner[] = {
        "  #####   #####   #####   #####   #####    #####  ",
        "  ##  ##  ##  ##  ##  ##  ##  ##  ##  ##  ##   ## ",
        "  #####   #####   #####   #####   #####   ##   ## ",
        "  ##  ##  ##  ##  ##  ##  ##  ##  ##  ##   ##  ## ",
        "  ##   ## ##   ## ##   ## ##   ## ##   ##   #####  ",
    };

    graphics_clear(default_color);
    uint32_t banner_rows = sizeof(banner) / sizeof(banner[0]);
    for (uint32_t i = 0; i < banner_rows; i++) {
        uint32_t length = graphics_strlen(banner[i]);
        uint32_t col = (VGA_WIDTH - length) / 2;
        graphics_print_at(3 + i, col, banner[i], graphics_make_color(VGA_COLOR_YELLOW, VGA_COLOR_BLUE));
    }

    if (title) {
        char line[64];
        uint32_t length = graphics_strlen(title);
        uint32_t col = (VGA_WIDTH - length) / 2;
        graphics_print_at(10, col, title, graphics_make_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLUE));
    }

    graphics_print_at(12, 8, "███████╗███████╗██╗  ██╗██╗  ██╗██╗  ██╗", graphics_make_color(VGA_COLOR_LIGHT_GRAY, VGA_COLOR_BLUE));
    graphics_print_at(13, 8, "██╔════╝██╔════╝██║  ██║██║ ██╔╝██║ ██╔╝", graphics_make_color(VGA_COLOR_LIGHT_GRAY, VGA_COLOR_BLUE));
    graphics_print_at(14, 8, "███████╗█████╗  ███████║█████╔╝ █████╔╝ ", graphics_make_color(VGA_COLOR_LIGHT_GRAY, VGA_COLOR_BLUE));
    graphics_print_at(15, 8, "╚════██║██╔══╝  ██╔══██║██╔═██╗ ██╔═██╗ ", graphics_make_color(VGA_COLOR_LIGHT_GRAY, VGA_COLOR_BLUE));
    graphics_print_at(16, 8, "███████║███████╗██║  ██║██║  ██╗██║  ██╗", graphics_make_color(VGA_COLOR_LIGHT_GRAY, VGA_COLOR_BLUE));
    graphics_print_at(17, 8, "╚══════╝╚══════╝╚═╝  ╚═╝╚═╝  ╚═╝╚═╝  ╚═╝", graphics_make_color(VGA_COLOR_LIGHT_GRAY, VGA_COLOR_BLUE));
}

void graphics_show_status(const char *text) {
    graphics_print_at(20, 0, "[STATUS] ", graphics_make_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    graphics_print_at(20, 9, text, graphics_make_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
}

void graphics_print_error(const char *text) {
    graphics_print_at(22, 0, "[ERROR]  ", graphics_make_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
    graphics_print_at(22, 9, text, graphics_make_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
}
