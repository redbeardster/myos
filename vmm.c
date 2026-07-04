#include "vmm.h"
#include "memory.h"

#include <stdint.h>

static uint64_t kernel_cr3;

static uint64_t *phys_to_table(uint64_t phys) {
    return (uint64_t *)(uintptr_t)(phys + memory_hhdm());
}

static uint64_t *walk_pte(uint64_t cr3, uint64_t virt, int create) {
    uint64_t *pml4 = phys_to_table(cr3 & ~0xFFFULL);
    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t *pdpt_ent = &pml4[pml4_idx];

    if (!(*pdpt_ent & PTE_PRESENT)) {
        if (!create) {
            return NULL;
        }
        uint64_t *new_pdpt = (uint64_t *)alloc_page();
        if (!new_pdpt) {
            return NULL;
        }
        for (int i = 0; i < 512; i++) {
            new_pdpt[i] = 0;
        }
        *pdpt_ent = virt_to_phys(new_pdpt) | PTE_PRESENT | PTE_WRITE | PTE_USER;
    }

    uint64_t *pdpt = phys_to_table(*pdpt_ent & ~0xFFFULL);
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t *pd_ent = &pdpt[pdpt_idx];

    if (!(*pd_ent & PTE_PRESENT)) {
        if (!create) {
            return NULL;
        }
        uint64_t *new_pd = (uint64_t *)alloc_page();
        if (!new_pd) {
            return NULL;
        }
        for (int i = 0; i < 512; i++) {
            new_pd[i] = 0;
        }
        *pd_ent = virt_to_phys(new_pd) | PTE_PRESENT | PTE_WRITE | PTE_USER;
    }

    uint64_t *pd = phys_to_table(*pd_ent & ~0xFFFULL);
    uint64_t pd_idx = (virt >> 21) & 0x1FF;
    uint64_t *pt_ent = &pd[pd_idx];

    if (!(*pt_ent & PTE_PRESENT)) {
        if (!create) {
            return NULL;
        }
        uint64_t *new_pt = (uint64_t *)alloc_page();
        if (!new_pt) {
            return NULL;
        }
        for (int i = 0; i < 512; i++) {
            new_pt[i] = 0;
        }
        *pt_ent = virt_to_phys(new_pt) | PTE_PRESENT | PTE_WRITE | PTE_USER;
    }

    uint64_t *pt = phys_to_table(*pt_ent & ~0xFFFULL);
    uint64_t pt_idx = (virt >> 12) & 0x1FF;
    return &pt[pt_idx];
}

static void free_pt(uint64_t *pt) {
    for (int i = 0; i < 512; i++) {
        if (pt[i] & PTE_PRESENT) {
            void *page = phys_to_virt(pt[i] & ~0xFFFULL);
            free_page(page);
        }
    }
    free_page(pt);
}

static void free_pd(uint64_t *pd) {
    for (int i = 0; i < 512; i++) {
        if (pd[i] & PTE_PRESENT) {
            free_pt(phys_to_table(pd[i] & ~0xFFFULL));
        }
    }
    free_page(pd);
}

static void free_pdpt(uint64_t *pdpt) {
    for (int i = 0; i < 512; i++) {
        if (pdpt[i] & PTE_PRESENT) {
            free_pd(phys_to_table(pdpt[i] & ~0xFFFULL));
        }
    }
    free_page(pdpt);
}

static void destroy_user_half(uint64_t cr3) {
    uint64_t *pml4 = phys_to_table(cr3 & ~0xFFFULL);
    for (int i = 0; i < 256; i++) {
        if (pml4[i] & PTE_PRESENT) {
            free_pdpt(phys_to_table(pml4[i] & ~0xFFFULL));
            pml4[i] = 0;
        }
    }
}

void vmm_init(void) {
    __asm__ volatile("mov %%cr3, %0" : "=r"(kernel_cr3));
}

uint64_t vmm_kernel_cr3(void) {
    return kernel_cr3;
}

int vmm_cr3_active(uint64_t cr3) {
    if (!cr3) {
        return 0;
    }
    uint64_t active_cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(active_cr3));
    return (active_cr3 & ~0xFFFULL) == (cr3 & ~0xFFFULL);
}

uint64_t vmm_aspace_create(void) {
    uint64_t *new_pml4 = (uint64_t *)alloc_page();
    if (!new_pml4) {
        return 0;
    }

    uint64_t *k_pml4 = phys_to_table(kernel_cr3 & ~0xFFFULL);
    for (int i = 0; i < 512; i++) {
        new_pml4[i] = 0;
    }
    for (int i = 256; i < 512; i++) {
        new_pml4[i] = k_pml4[i];
    }

    return virt_to_phys(new_pml4);
}

void vmm_aspace_destroy(uint64_t cr3) {
    if (!cr3 || cr3 == kernel_cr3) {
        return;
    }

    destroy_user_half(cr3);
    free_page(phys_to_virt(cr3 & ~0xFFFULL));
}

void vmm_switch(uint64_t cr3) {
    if (!cr3) {
        cr3 = kernel_cr3;
    }
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

int vmm_map(uint64_t cr3, uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t *pte = walk_pte(cr3, virt, 1);
    if (!pte) {
        return -1;
    }
    *pte = (phys & ~0xFFFULL) | flags | PTE_PRESENT | PTE_USER;
    if (vmm_cr3_active(cr3)) {
        vmm_flush(virt);
    }
    return 0;
}

int vmm_unmap(uint64_t cr3, uint64_t virt, uint64_t *phys_out) {
    uint64_t *pte = walk_pte(cr3, virt, 0);
    if (!pte || !(*pte & PTE_PRESENT)) {
        return -1;
    }
    if (phys_out) {
        *phys_out = *pte & ~0xFFFULL;
    }
    *pte = 0;
    if (vmm_cr3_active(cr3)) {
        vmm_flush(virt);
    }
    return 0;
}

void vmm_flush(uint64_t virt) {
    __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
}
