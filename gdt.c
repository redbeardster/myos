#include "gdt.h"

#include <stdint.h>

struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t base_mid;
    uint8_t access;
    uint8_t granularity;
    uint8_t base_high;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

typedef struct {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed)) tss_t;

static struct gdt_entry gdt[9];
static struct gdt_ptr gp;
static tss_t tss;

extern void gdt_load(uint64_t ptr);
extern void load_tss(uint16_t selector);

static void gdt_set_gate(unsigned idx, uint32_t base, uint32_t limit,
                         uint8_t access, uint8_t gran) {
    gdt[idx].base_low = (uint16_t)(base & 0xFFFF);
    gdt[idx].base_mid = (uint8_t)((base >> 16) & 0xFF);
    gdt[idx].base_high = (uint8_t)((base >> 24) & 0xFF);
    gdt[idx].limit_low = (uint16_t)(limit & 0xFFFF);
    gdt[idx].granularity = (uint8_t)((limit >> 16) & 0x0F);
    gdt[idx].granularity |= gran & 0xF0;
    gdt[idx].access = access;
}

static void gdt_set_tss(unsigned idx, uint64_t base, uint32_t limit) {
    gdt[idx].limit_low = (uint16_t)(limit & 0xFFFF);
    gdt[idx].base_low = (uint16_t)(base & 0xFFFF);
    gdt[idx].base_mid = (uint8_t)((base >> 16) & 0xFF);
    gdt[idx].base_high = (uint8_t)((base >> 24) & 0xFF);
    gdt[idx].access = 0x89;
    gdt[idx].granularity = (uint8_t)((limit >> 16) & 0x0F);

    uint64_t base_upper = base >> 32;
    gdt[idx + 1].limit_low = (uint16_t)(base_upper & 0xFFFF);
    gdt[idx + 1].base_low = (uint16_t)((base_upper >> 16) & 0xFFFF);
    gdt[idx + 1].base_mid = (uint8_t)((base_upper >> 32) & 0xFF);
    gdt[idx + 1].base_high = (uint8_t)((base_upper >> 40) & 0xFF);
    gdt[idx + 1].access = 0;
    gdt[idx + 1].granularity = 0;
}

void tss_set_rsp0(uint64_t rsp0) {
    tss.rsp0 = rsp0;
}

void gdt_init(void) {
    gdt_set_gate(0, 0, 0, 0, 0);

    gdt_set_gate(1, 0, 0, 0x9A, 0xA0);
    gdt_set_gate(2, 0, 0, 0x92, 0xA0);
    gdt_set_gate(3, 0, 0, 0xFA, 0xA0);
    gdt_set_gate(4, 0, 0, 0xF2, 0xA0);
    gdt_set_gate(5, 0, 0, 0x9A, 0x00);
    gdt_set_gate(6, 0, 0, 0x92, 0x00);

    tss.iomap_base = sizeof(tss_t);
    gdt_set_tss(7, (uint64_t)(uintptr_t)&tss, sizeof(tss_t) - 1);

    gp.limit = (uint16_t)(sizeof(gdt) - 1);
    gp.base = (uint64_t)(uintptr_t)&gdt;
    gdt_load((uint64_t)(uintptr_t)&gp);
    load_tss(TSS_SELECTOR);
}
