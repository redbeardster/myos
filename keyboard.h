#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>

struct lwkt_thread;

void keyboard_init(void);
void keyboard_irq_handler(void);
void keyboard_set_reader(struct lwkt_thread *t);
void keyboard_clear_reader(struct lwkt_thread *t);

int keyboard_has_scancode(void);
uint8_t keyboard_read_scancode(void);

/* Non-blocking: returns 0 if no translated key available. */
char keyboard_poll_char(void);

/* Blocking read of a printable/control character. */
char keyboard_read_char(void);

#endif
