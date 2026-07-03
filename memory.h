// memory.h
#ifndef MEMORY_H
#define MEMORY_H

#include <stdint.h>
#include <stddef.h>

// Инициализация менеджера памяти
void memory_init(void);

// Выделение страницы памяти (4KB)
void* alloc_page(void);

// Освобождение страницы памяти
void free_page(void* addr);

// Выделение непрерывного блока памяти (в страницах)
void* alloc_pages(size_t count);

// Освобождение блока памяти
void free_pages(void* addr, size_t count);

// Получение информации о памяти
void print_memory_info(void);

#endif
