#include "user.h"

#include "gdt.h"
#include "lwkt.h"
#include "memory.h"
#include "myos_abi.h"
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

int user_stack_alloc(struct proc *p, uint64_t *rsp_out, uint64_t *base_out) {
    if (!p || !p->cr3 || !rsp_out || !base_out) {
        return -1;
    }

    if (p->stack_next <= MYOS_USER_STACK_BASE + PAGE_SIZE) {
        return -2;
    }
    if (p->stack_next - PAGE_SIZE < p->heap_next) {
        return -3;
    }

    void *page = alloc_page();
    if (!page) {
        return -4;
    }

    uint64_t base = p->stack_next - PAGE_SIZE;
    p->stack_next = base;

    if (vmm_map(p->cr3, base, virt_to_phys(page), PTE_WRITE) != 0) {
        p->stack_next += PAGE_SIZE;
        free_page(page);
        return -5;
    }

    *base_out = base;
    *rsp_out = base + PAGE_SIZE - 16;
    return 0;
}

void user_stack_free(struct proc *p, uint64_t stack_base) {
    if (!p || !p->cr3 || stack_base < MYOS_USER_STACK_BASE) {
        return;
    }

    uint64_t phys;
    if (vmm_unmap(p->cr3, stack_base, &phys) == 0) {
        free_page(phys_to_virt(phys));
    }
}

extern void user_enter_asm(uint64_t rip, uint64_t rsp, uint64_t arg, uint64_t *save_kernel_rsp);

void user_enter(uint64_t rip, uint64_t rsp, uint64_t arg, uint64_t *save_kernel_rsp) {
    struct lwkt_thread *t = lwkt_curthread();
    if (t) {
        uint64_t top = (uint64_t)(uintptr_t)t->stack + STACK_SIZE;
        tss_set_rsp0(top & ~0xFULL);
    }

    user_enter_asm(rip, rsp, arg, save_kernel_rsp);

    for (;;) {
        __asm__ volatile("hlt");
    }
}
