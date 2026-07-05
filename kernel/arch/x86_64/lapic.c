#include "lapic.h"

#include "cpu.h"
#include "memory.h"
#include "vmm.h"

#include <stdint.h>

#define MSR_IA32_APIC_BASE 0x1B

#define APIC_ID          0x020
#define APIC_EOI         0x0B0
#define APIC_SVR         0x0F0
#define APIC_LVT_TIMER   0x320
#define APIC_TIMER_DIV   0x3E0
#define APIC_TIMER_INIT  0x380
#define APIC_TIMER_CUR   0x390
#define APIC_ICR_LOW     0x300
#define APIC_ICR_HIGH    0x310

#define APIC_SVR_ENABLE  (1u << 8)
#define APIC_ICR_SEND_PENDING (1u << 12)
#define APIC_ICR_DEST_ALL_BUT_SELF (3u << 18)

static volatile uint32_t *apic_mmio;
static uint32_t lapic_id_cached;

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t value) {
    uint32_t lo = (uint32_t)value;
    uint32_t hi = (uint32_t)(value >> 32);
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

static uint32_t apic_read(uint32_t reg) {
    return apic_mmio[reg / 4];
}

static void apic_write(uint32_t reg, uint32_t value) {
    apic_mmio[reg / 4] = value;
}

void lapic_init(void) {
    uint64_t msr = rdmsr(MSR_IA32_APIC_BASE);
    uint64_t phys = msr & 0xFFFFFFFFFFFFF000ULL;
    msr = phys | (1ULL << 11) | (msr & (1ULL << 10));
    wrmsr(MSR_IA32_APIC_BASE, msr);

    uint64_t virt = phys + memory_hhdm();
    vmm_map(vmm_kernel_cr3(), virt & ~0xFFFULL, phys & ~0xFFFULL, PTE_WRITE);
    apic_mmio = (volatile uint32_t *)(uintptr_t)virt;
    apic_write(APIC_SVR, apic_read(APIC_SVR) | APIC_SVR_ENABLE);
    lapic_id_cached = apic_read(APIC_ID) >> 24;
}

uint32_t lapic_id(void) {
    return lapic_id_cached;
}

void lapic_timer_start(void) {
    if (!apic_mmio) {
        lapic_init();
    }

    apic_write(APIC_LVT_TIMER, LAPIC_TIMER_VECTOR | (1u << 17));
    apic_write(APIC_TIMER_DIV, 0x3);
    apic_write(APIC_TIMER_INIT, 100000);
}

void lapic_timer_stop(void) {
    if (!apic_mmio) {
        return;
    }
    apic_write(APIC_LVT_TIMER, 1u << 16);
}

void lapic_eoi(void) {
    if (apic_mmio) {
        apic_write(APIC_EOI, 0);
    }
}

static void lapic_icr_wait(void) {
    while (apic_read(APIC_ICR_LOW) & APIC_ICR_SEND_PENDING) {
        __asm__ volatile("pause");
    }
}

void lapic_ipi_send(uint32_t dest_lapic_id, uint8_t vector) {
    if (!apic_mmio) {
        lapic_init();
    }

    lapic_icr_wait();
    apic_write(APIC_ICR_HIGH, dest_lapic_id << 24);
    apic_write(APIC_ICR_LOW, vector);
    lapic_icr_wait();
}

void lapic_ipi_reschedule_others(void) {
    if (!apic_mmio) {
        lapic_init();
    }
    if (cpu_online_count() <= 1) {
        return;
    }

    lapic_icr_wait();
    apic_write(APIC_ICR_HIGH, 0);
    apic_write(APIC_ICR_LOW, LAPIC_IPI_RESCHED_VECTOR | APIC_ICR_DEST_ALL_BUT_SELF);
    lapic_icr_wait();
}
