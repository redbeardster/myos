// terminal.c
#include "terminal.h"
#include <stdint.h>
#include <stddef.h>

// Размеры экрана (текстовый режим VGA)
static const size_t VGA_WIDTH = 80;
static const size_t VGA_HEIGHT = 25;

// Указатель на видеопамять
static uint16_t* const VGA_MEMORY = (uint16_t*) 0xB8000;

// Текущие позиции курсора
static size_t terminal_row;
static size_t terminal_column;
static uint8_t terminal_color;

// Создание VGA-атрибута (цвет фона + цвет текста)
static inline uint8_t vga_entry_color(enum vga_color fg, enum vga_color bg) {
    return fg | bg << 4;
}

// Создание VGA-символа (символ + атрибут)
static inline uint16_t vga_entry(unsigned char uc, uint8_t color) {
    return (uint16_t) uc | (uint16_t) color << 8;
}

// Инициализация терминала
void terminal_initialize(void) {
    terminal_row = 0;
    terminal_column = 0;
    terminal_color = vga_entry_color(COLOR_WHITE, COLOR_BLACK);
    terminal_clear();
}

// Очистка экрана
void terminal_clear(void) {
    for (size_t y = 0; y < VGA_HEIGHT; y++) {
        for (size_t x = 0; x < VGA_WIDTH; x++) {
            const size_t index = y * VGA_WIDTH + x;
            VGA_MEMORY[index] = vga_entry(' ', terminal_color);
        }
    }
    terminal_row = 0;
    terminal_column = 0;
}

// Установка цвета
void terminal_setcolor(uint8_t color) {
    terminal_color = color;
}

// Перемещение курсора (прокрутка при необходимости)
static void terminal_scroll(void) {
    if (terminal_row >= VGA_HEIGHT) {
        // Сдвигаем все строки вверх
        for (size_t y = 0; y < VGA_HEIGHT - 1; y++) {
            for (size_t x = 0; x < VGA_WIDTH; x++) {
                const size_t src_idx = (y + 1) * VGA_WIDTH + x;
                const size_t dst_idx = y * VGA_WIDTH + x;
                VGA_MEMORY[dst_idx] = VGA_MEMORY[src_idx];
            }
        }

        // Очищаем последнюю строку
        for (size_t x = 0; x < VGA_WIDTH; x++) {
            const size_t idx = (VGA_HEIGHT - 1) * VGA_WIDTH + x;
            VGA_MEMORY[idx] = vga_entry(' ', terminal_color);
        }

        terminal_row = VGA_HEIGHT - 1;
    }
}

// Вывод символа
void terminal_putchar(char c) {
    if (c == '\n') {
        terminal_column = 0;
        terminal_row++;
        terminal_scroll();
        return;
    }

    if (c == '\r') {
        terminal_column = 0;
        return;
    }

    if (c == '\t') {
        // Табуляция - 4 пробела
        for (int i = 0; i < 4; i++) {
            terminal_putchar(' ');
        }
        return;
    }

    if (c == '\b') {
        // Backspace - удаляем символ
        if (terminal_column > 0) {
            terminal_column--;
            const size_t idx = terminal_row * VGA_WIDTH + terminal_column;
            VGA_MEMORY[idx] = vga_entry(' ', terminal_color);
        }
        return;
    }

    // Обычный символ
    const size_t idx = terminal_row * VGA_WIDTH + terminal_column;
    VGA_MEMORY[idx] = vga_entry(c, terminal_color);

    terminal_column++;
    if (terminal_column >= VGA_WIDTH) {
        terminal_column = 0;
        terminal_row++;
        terminal_scroll();
    }
}

// Вывод строки
void terminal_writestring(const char* data) {
    for (size_t i = 0; data[i] != '\0'; i++) {
        terminal_putchar(data[i]);
    }
}

// Вывод числа в десятичном формате
void terminal_write_dec(uint32_t n) {
    if (n == 0) {
        terminal_putchar('0');
        return;
    }

    // Буфер для обратного порядка цифр
    char buffer[32];
    int i = 0;

    while (n > 0) {
        buffer[i++] = '0' + (n % 10);
        n /= 10;
    }

    // Выводим в обратном порядке
    while (i > 0) {
        terminal_putchar(buffer[--i]);
    }
}

// Вывод числа в шестнадцатеричном формате
void terminal_write_hex(uint32_t n) {
    terminal_writestring("0x");

    if (n == 0) {
        terminal_putchar('0');
        return;
    }

    // Буфер для HEX-символов
    char hex_chars[] = "0123456789ABCDEF";
    char buffer[8];  // 32 бита = 8 hex-цифр
    int i = 0;

    while (n > 0) {
        buffer[i++] = hex_chars[n & 0xF];
        n >>= 4;
    }

    // Выводим в обратном порядке
    while (i > 0) {
        terminal_putchar(buffer[--i]);
    }
}

// Вывод числа в двоичном формате
void terminal_write_bin(uint32_t n) {
    terminal_writestring("0b");

    // Выводим все 32 бита
    int started = 0;
    for (int i = 31; i >= 0; i--) {
        uint32_t bit = (n >> i) & 1;
        if (bit || started || i == 0) {
            terminal_putchar(bit ? '1' : '0');
            started = 1;
        }
    }
}
