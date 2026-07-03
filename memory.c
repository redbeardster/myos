// memory.c
#include "memory.h"
#include "terminal.h"

#define PAGE_SIZE 4096
#define MEMORY_START 0x100000  // 1MB
#define MEMORY_END 0x2000000   // 32MB
#define TOTAL_PAGES ((MEMORY_END - MEMORY_START) / PAGE_SIZE)

static uint8_t page_bitmap[TOTAL_PAGES / 8 + 1];

void memory_init(void) {
    terminal_writestring("Initializing memory manager...\n");

    // Инициализируем битовую карту
    for (int i = 0; i < sizeof(page_bitmap); i++) {
        page_bitmap[i] = 0xFF;  // Все страницы заняты
    }

    // Освобождаем все страницы
    for (uint32_t i = 0; i < TOTAL_PAGES; i++) {
        page_bitmap[i / 8] &= ~(1 << (i % 8));  // Устанавливаем бит в 0 (свободно)
    }

    terminal_writestring("Total pages: ");
    terminal_write_dec(TOTAL_PAGES);
    terminal_writestring("\n");

    terminal_writestring("Memory manager initialized!\n");
}

void* alloc_page(void) {
    // Ищем свободную страницу
    for (uint32_t i = 0; i < TOTAL_PAGES; i++) {
        if (!(page_bitmap[i / 8] & (1 << (i % 8)))) {
            // Помечаем как занятую
            page_bitmap[i / 8] |= (1 << (i % 8));
            return (void*)(MEMORY_START + i * PAGE_SIZE);
        }
    }

    terminal_writestring("ERROR: Out of memory!\n");
    return NULL;
}

void free_page(void* addr) {
    uint32_t page_num = ((uint32_t)addr - MEMORY_START) / PAGE_SIZE;
    if (page_num < TOTAL_PAGES) {
        page_bitmap[page_num / 8] &= ~(1 << (page_num % 8));
    }
}
