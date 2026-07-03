// interrupt.h
#ifndef INTERRUPT_H
#define INTERRUPT_H

#include <stdint.h>

// Инициализация IDT
void idt_init(void);

// Обработчик прерываний (общий)
void interrupt_handler(uint32_t int_no, uint32_t err_code);

// Установка обработчика для конкретного прерывания
void set_interrupt_handler(uint8_t num, void* handler);

#endif
