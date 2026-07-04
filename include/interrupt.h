#ifndef INTERRUPT_H
#define INTERRUPT_H

#include <stdint.h>

void idt_init(void);
void pic_init(void);
void pic_eoi(uint8_t irq);
void interrupts_enable(void);
void interrupt_handler(uint64_t int_no, uint64_t err_code);

void timer_init(void);
void timer_interrupt_handler(void);
int timer_get_ticks(void);
int timer_get_switch_count(void);
void timer_reset_stats(void);
int timer_is_enabled(void);
void timer_set_enabled(int enabled);

#endif
