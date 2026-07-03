// keyboard.c
#include "terminal.h"

#define KEYBOARD_PORT 0x60
#define KEYBOARD_STATUS_PORT 0x64

// Простая таблица scancode -> ASCII
static const char scancode_to_ascii[] = {
    0,   0,   '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 0,
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0,   'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0,
    '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' '
};

// Чтение scancode с клавиатуры
unsigned char keyboard_read_scancode(void) {
    // Ждём, пока данные готовы
    while ((inb(KEYBOARD_STATUS_PORT) & 1) == 0);
    return inb(KEYBOARD_PORT);
}

// Обработчик клавиатуры
void keyboard_handler(void) {
    unsigned char scancode = keyboard_read_scancode();

    // Проверяем, что это нажатие клавиши (а не отпускание)
    if (scancode < 0x80) {
        char c = scancode_to_ascii[scancode];
        if (c != 0) {
            terminal_putchar(c);
        }
    }
}
