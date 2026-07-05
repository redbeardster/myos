#ifndef GDT_H
#define GDT_H

#include <stdint.h>

#define TSS_SELECTOR 0x38

void gdt_init(void);
void gdt_reload(void);
void gdt_load_tss(uint32_t cpu_id);
uint16_t gdt_tss_selector(uint32_t cpu_id);
void tss_set_rsp0(uint64_t rsp0);

#endif
