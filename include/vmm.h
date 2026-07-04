#ifndef VMM_H
#define VMM_H

#include <stdint.h>

#define PTE_PRESENT (1ULL << 0)
#define PTE_WRITE   (1ULL << 1)
#define PTE_USER    (1ULL << 2)

void vmm_init(void);

uint64_t vmm_kernel_cr3(void);
int vmm_cr3_active(uint64_t cr3);
uint64_t vmm_aspace_create(void);
void vmm_aspace_destroy(uint64_t cr3);

void vmm_switch(uint64_t cr3);

int vmm_map(uint64_t cr3, uint64_t virt, uint64_t phys, uint64_t flags);
int vmm_unmap(uint64_t cr3, uint64_t virt, uint64_t *phys_out);
void vmm_flush(uint64_t virt);

#endif
