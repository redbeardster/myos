#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>

void keyboard_init(void);
void keyboard_irq_handler(void);

/* Raw scancode queue (IRQ producer, kbdd consumer). */
int keyboard_scancode_pending(void);
int keyboard_pop_scancode(uint8_t *scancode);
char keyboard_translate_scancode(uint8_t scancode);

#endif
