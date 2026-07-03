// terminal.h
#ifndef TERMINAL_H
#define TERMINAL_H

#include <stdint.h>
#include <stddef.h>

// Цвета текста (VGA)
enum vga_color {
    COLOR_BLACK = 0,
    COLOR_BLUE = 1,
    COLOR_GREEN = 2,
    COLOR_CYAN = 3,
    COLOR_RED = 4,
    COLOR_MAGENTA = 5,
    COLOR_BROWN = 6,
    COLOR_LIGHT_GREY = 7,
    COLOR_DARK_GREY = 8,
    COLOR_LIGHT_BLUE = 9,
    COLOR_LIGHT_GREEN = 10,
    COLOR_LIGHT_CYAN = 11,
    COLOR_LIGHT_RED = 12,
    COLOR_LIGHT_MAGENTA = 13,
    COLOR_LIGHT_BROWN = 14,
    COLOR_WHITE = 15,
};

// Инициализация терминала
void terminal_initialize(void);

// Установка цвета текста
void terminal_setcolor(uint8_t color);

// Вывод символа
void terminal_putchar(char c);

// Вывод строки
void terminal_writestring(const char* data);

// Вывод числа в десятичном формате
void terminal_write_dec(uint32_t n);

// Вывод числа в шестнадцатеричном формате
void terminal_write_hex(uint32_t n);

// Вывод числа в двоичном формате
void terminal_write_bin(uint32_t n);

// Очистка экрана
void terminal_clear(void);

#endif
