#include "user.h"

#include "gdt.h"
#include "lwkt.h"
#include "memory.h"
#include "proc.h"
#include "vmm.h"

#include <stdint.h>

void *user_page_alloc(void) {
    struct proc *p = proc_current();
    if (!p || !p->cr3) {
        return NULL;
    }

    void *page = alloc_page();
    if (!page) {
        return NULL;
    }

    uint64_t virt = p->heap_next;
    p->heap_next += PAGE_SIZE;

    if (vmm_map(p->cr3, virt, virt_to_phys(page), PTE_WRITE) != 0) {
        free_page(page);
        return NULL;
    }

    return (void *)(uintptr_t)virt;
}

void user_free_page(void *user_addr) {
    struct proc *p = proc_current();
    if (!p || !p->cr3 || !user_addr) {
        return;
    }

    uint64_t phys;
    if (vmm_unmap(p->cr3, (uint64_t)(uintptr_t)user_addr, &phys) != 0) {
        return;
    }

    free_page(phys_to_virt(phys));
}

extern void user_enter_asm(uint64_t rip, uint64_t rsp, uint64_t *save_kernel_rsp);

void user_enter(uint64_t rip, uint64_t rsp, uint64_t *save_kernel_rsp) {
    struct lwkt_thread *t = lwkt_curthread();
    if (t) {
        uint64_t top = (uint64_t)(uintptr_t)t->stack + STACK_SIZE;
        tss_set_rsp0(top & ~0xFULL);
    }

    user_enter_asm(rip, rsp, save_kernel_rsp);

    for (;;) {
        __asm__ volatile("hlt");
    }
}
