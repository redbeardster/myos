// gdt.h
#ifndef GDT_H
#define GDT_H

#include <stdint.h>

// Загрузка GDT (вызывается из C, передаём указатель на структуру gdt_ptr)
void gdt_load(uint32_t gdt_ptr);

// Инициализация GDT
void gdt_init(void);

#endif
