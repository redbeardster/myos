#include "cpu.h"
#include "console.h"
#include "gdt.h"
#include "interrupt.h"
#include "io.h"
#include "lapic.h"
#include "lwkt.h"
#include "smp.h"

#include <limine.h>
#include <stdint.h>

static struct cpu cpus[MAX_CPUS];
static uint32_t cpu_count;
static volatile int aps_waiting;
static volatile int smp_go;

static inline void wrmsr_gs(uint64_t addr) {
    uint32_t lo = (uint32_t)addr;
    uint32_t hi = (uint32_t)(addr >> 32);
    __asm__ volatile("wrmsr" : : "c"((uint32_t)0xC0000101), "a"(lo), "d"(hi));
}

struct cpu *cpu_current(void) {
    struct cpu *cpu;
    __asm__ volatile("mov %%gs:0, %0" : "=r"(cpu));
    return cpu;
}

struct cpu *cpu_by_id(uint32_t id) {
    if (id >= cpu_count) {
        return NULL;
    }
    return &cpus[id];
}

uint32_t cpu_online_count(void) {
    return cpu_count;
}

int cpu_is_bsp(void) {
    struct cpu *c = cpu_current();
    return c && c->bsp;
}

int cpu_index_of_thread(struct lwkt_thread *t) {
    if (!t) {
        return -1;
    }
    for (uint32_t i = 0; i < cpu_count; i++) {
        if (cpus[i].current == t) {
            return (int)i;
        }
    }
    return -1;
}

void cpu_set_gs(struct cpu *cpu) {
    if (!cpu) {
        return;
    }
    cpu->self = cpu;
    wrmsr_gs((uint64_t)(uintptr_t)cpu);
}

static struct cpu *cpu_alloc(uint32_t lapic_id, int bsp) {
    if (cpu_count >= MAX_CPUS) {
        return NULL;
    }

    struct cpu *c = &cpus[cpu_count++];
    c->self = c;
    c->id = cpu_count - 1;
    c->lapic_id = lapic_id;
    c->online = 1;
    c->bsp = bsp;
    c->current = NULL;
    c->preempt_requested = 0;
    c->sched_active = 0;
    c->bootstrap_rsp = 0;
    c->switches = 0;
    return c;
}

void cpu_init_bsp(void) {
    cpu_count = 0;
    aps_waiting = 0;
    smp_go = 0;

    lapic_init();
    struct cpu *bsp = cpu_alloc(lapic_id(), 1);
    cpu_set_gs(bsp);
}

struct cpu *cpu_init_ap(uint32_t lapic_id) {
    struct cpu *c = cpu_alloc(lapic_id, 0);
    if (!c) {
        return NULL;
    }
    cpu_set_gs(c);
    lapic_init();
    gdt_load_tss(c->id);
    return c;
}

void smp_lapic_entry(struct LIMINE_MP(info) *info) {
    __asm__ volatile("cli");

    gdt_reload();
    idt_reload();

    if (!info) {
        for (;;) {
            __asm__ volatile("hlt");
        }
    }

    cpu_init_ap(info->lapic_id);

    __atomic_fetch_add(&aps_waiting, 1, __ATOMIC_SEQ_CST);

    while (!__atomic_load_n(&smp_go, __ATOMIC_ACQUIRE)) {
        __asm__ volatile("pause");
    }

    interrupts_enable();
    lwkt_cpu_init_idle();
    lapic_timer_start();
    lwkt_sched_enable();
    lwkt_ap_bootstrap();
}

void smp_init(volatile struct LIMINE_MP(response) *smp) {
    if (!smp || smp->cpu_count <= 1) {
        console_writestring("SMP: single CPU (no APs)\n");
        return;
    }

    console_writestring("SMP: booting ");
    console_write_dec(smp->cpu_count - 1);
    console_writestring(" AP(s), BSP lapic=");
    console_write_dec(smp->bsp_lapic_id);
    console_putchar('\n');

    for (uint64_t i = 0; i < smp->cpu_count; i++) {
        struct LIMINE_MP(info) *info = smp->cpus[i];
        if (!info) {
            continue;
        }
        if (info->lapic_id == smp->bsp_lapic_id) {
            continue;
        }
        __atomic_store_n(&info->goto_address, smp_lapic_entry, __ATOMIC_RELEASE);
    }

    while ((uint32_t)__atomic_load_n(&aps_waiting, __ATOMIC_SEQ_CST) + 1 < smp->cpu_count) {
        __asm__ volatile("pause");
    }

    console_writestring("SMP: all CPUs online (");
    console_write_dec(cpu_count);
    console_writestring(" total)\n");
}

void smp_release_aps(void) {
    if (cpu_online_count() > 1) {
        timer_set_enabled(0);
        outb(0x21, (uint8_t)(inb(0x21) | 1));
        lapic_timer_start();
    } else if (!timer_is_enabled()) {
        timer_init();
    }

    lwkt_sched_enable();
}

void smp_start_aps(void) {
    __atomic_store_n(&smp_go, 1, __ATOMIC_RELEASE);
}

static const char *cpu_state_name(struct cpu *c) {
    if (!c || !c->current) {
        return "-";
    }
    switch (c->current->state) {
        case THREAD_READY: return "ready";
        case THREAD_RUNNING: return "running";
        case THREAD_BLOCKED: return "blocked";
        case THREAD_TERMINATED: return "dead";
        default: return "?";
    }
}

void cpu_list(void) {
    console_writestring("\nCPU  Lapic  BSP  Sched  Switches  Current thread\n");
    console_writestring("---  -----  ---  -----  --------  --------------\n");

    for (uint32_t i = 0; i < cpu_count; i++) {
        struct cpu *c = &cpus[i];
        console_write_dec(c->id);
        console_writestring("    ");
        console_write_dec(c->lapic_id);
        console_writestring("      ");
        console_writestring(c->bsp ? "yes" : "no ");
        console_writestring("   ");
        console_writestring(c->sched_active ? "on " : "off");
        console_writestring("    ");
        console_write_dec(c->switches);
        console_writestring("        ");

        if (c->current && c->current->id != 0) {
            console_writestring(c->current->name);
            console_writestring(" (");
            console_writestring(cpu_state_name(c));
            console_putchar(')');
        } else if (c->current) {
            console_writestring(c->current->name);
            console_writestring(" (");
            console_writestring(cpu_state_name(c));
            console_putchar(')');
        } else {
            console_writestring("-");
        }
        console_putchar('\n');
    }

    console_writestring("\n");
    console_write_dec(cpu_count);
    console_writestring(" CPU(s) online\n");
}
