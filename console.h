#ifndef CONSOLE_H
#define CONSOLE_H

#include <stdint.h>
#include <limine.h>

#define SCREEN_COLS 80
#define SCREEN_ROWS 45

enum console_color {
    COLOR_BLACK = 0,
    COLOR_BLUE,
    COLOR_GREEN,
    COLOR_CYAN,
    COLOR_RED,
    COLOR_MAGENTA,
    COLOR_BROWN,
    COLOR_LIGHT_GREY,
    COLOR_DARK_GREY,
    COLOR_LIGHT_BLUE,
    COLOR_LIGHT_GREEN,
    COLOR_LIGHT_CYAN,
    COLOR_LIGHT_RED,
    COLOR_LIGHT_MAGENTA,
    COLOR_LIGHT_BROWN,
    COLOR_WHITE,
};

void console_init(struct limine_framebuffer *fb);
void console_setcolor(enum console_color fg, enum console_color bg);
void console_putchar(char c);
void console_writestring(const char *str);
void console_write_dec(uint64_t n);
void console_write_hex(uint64_t n);
void console_clear(void);
void console_set_cursor(unsigned col, unsigned row);
void console_save_cursor(void);
void console_restore_cursor(void);
void console_draw_at(unsigned col, unsigned row, char c);
void console_write_at(unsigned col, unsigned row, const char *str);
void console_write_dec_at(unsigned col, unsigned row, uint64_t n, int width);
void console_fill_row(unsigned row, unsigned col_start, unsigned col_end, char c);

#endif
