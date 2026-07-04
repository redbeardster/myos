#ifndef GDT_H
#define GDT_H

#include <stdint.h>

#define TSS_SELECTOR 0x38

void gdt_init(void);
void tss_set_rsp0(uint64_t rsp0);

#endif
