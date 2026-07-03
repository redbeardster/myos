// gdt.c
#include "gdt.h"
#include <stdint.h>

// Структура дескриптора GDT
struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t base_middle;
    uint8_t access;
    uint8_t granularity;
    uint8_t base_high;
} __attribute__((packed));

// Структура для загрузки GDT (указатель на таблицу)
struct gdt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

// GDT с 5 дескрипторами
static struct gdt_entry gdt[5];
static struct gdt_ptr gp;

// Функция для установки дескриптора
static void gdt_set_gate(int num, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    gdt[num].base_low = (base & 0xFFFF);
    gdt[num].base_middle = (base >> 16) & 0xFF;
    gdt[num].base_high = (base >> 24) & 0xFF;

    gdt[num].limit_low = (limit & 0xFFFF);
    gdt[num].granularity = (limit >> 16) & 0x0F;
    gdt[num].granularity |= (gran & 0xF0);

    gdt[num].access = access;
}

// Инициализация GDT
void gdt_init(void) {
    // Устанавливаем границу GDT
    gp.limit = (sizeof(struct gdt_entry) * 5) - 1;
    gp.base = (uint32_t)&gdt;

    // Нулевой дескриптор (обязателен)
    gdt_set_gate(0, 0, 0, 0, 0);

    // Дескриптор кода ядра (сегмент 0x08)
    gdt_set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xCF);

    // Дескриптор данных ядра (сегмент 0x10)
    gdt_set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF);

    // Дескриптор кода пользователя (сегмент 0x1B)
    gdt_set_gate(3, 0, 0xFFFFFFFF, 0xFA, 0xCF);

    // Дескриптор данных пользователя (сегмент 0x23)
    gdt_set_gate(4, 0, 0xFFFFFFFF, 0xF2, 0xCF);

    // Загружаем GDT
    gdt_load((uint32_t)&gp);
}
