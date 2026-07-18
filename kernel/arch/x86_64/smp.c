#include "cpu.h"
#include "console.h"
#include "gdt.h"
#include "interrupt.h"
#include "io.h"
#include "lapic.h"
#include "lwkt.h"
#include "smp.h"
#include "spinlock.h"

#include <limine.h>
#include <stdint.h>

static struct cpu cpus[MAX_CPUS];
static uint32_t cpu_count;
static spinlock_t cpu_list_lock;
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
    struct cpu *cpu = NULL;
    spin_lock(&cpu_list_lock);
    if (id < cpu_count) {
        cpu = &cpus[id];
    }
    spin_unlock(&cpu_list_lock);
    return cpu;
}

uint32_t cpu_online_count(void) {
    uint32_t n;
    spin_lock(&cpu_list_lock);
    n = cpu_count;
    spin_unlock(&cpu_list_lock);
    return n;
}

int cpu_is_bsp(void) {
    struct cpu *c = cpu_current();
    return c && c->bsp;
}

int cpu_index_of_thread(struct lwkt_thread *t) {
    if (!t) {
        return -1;
    }
    spin_lock(&cpu_list_lock);
    uint32_t ncpu = cpu_count;
    for (uint32_t i = 0; i < ncpu; i++) {
        if (cpus[i].current == t) {
            spin_unlock(&cpu_list_lock);
            return (int)i;
        }
    }
    spin_unlock(&cpu_list_lock);
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
    spin_lock(&cpu_list_lock);
    uint32_t idx = cpu_count;
    if (idx >= MAX_CPUS) {
        spin_unlock(&cpu_list_lock);
        return NULL;
    }

    struct cpu *c = &cpus[idx];
    c->self = c;
    c->id = idx;
    c->lapic_id = lapic_id;
    c->online = 1;
    c->bsp = bsp;
    c->current = NULL;
    c->preempt_requested = 0;
    c->sched_active = 0;
    c->bootstrap_rsp = 0;
    c->switches = 0;
    c->steals = 0;
    c->same_proc_pulls = 0;
    c->ipi_rx = 0;
    for (int i = 0; i < MAX_PRIORITY; i++) {
        c->run_queues[i] = NULL;
    }
    cpu_count = idx + 1;
    spin_unlock(&cpu_list_lock);
    return c;
}

void cpu_init_bsp(void) {
    spin_init(&cpu_list_lock);
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

static void strcpy_snap(char *dst, const char *src, int cap) {
    int i = 0;
    if (!dst || cap <= 0) {
        return;
    }
    while (src && src[i] && i < cap - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void write_padded(const char *s, int width) {
    int n = 0;
    if (!s) {
        s = "-";
    }
    while (s[n] && n < width) {
        console_putchar(s[n]);
        n++;
    }
    while (n < width) {
        console_putchar(' ');
        n++;
    }
}

static void write_u64_padded(uint64_t v, int width) {
    char buf[24];
    int n = 0;
    if (v == 0) {
        buf[n++] = '0';
    } else {
        char tmp[24];
        int t = 0;
        while (v > 0 && t < (int)sizeof(tmp)) {
            tmp[t++] = (char)('0' + (v % 10));
            v /= 10;
        }
        while (t > 0) {
            buf[n++] = tmp[--t];
        }
    }
    for (int i = n; i < width; i++) {
        console_putchar(' ');
    }
    for (int i = 0; i < n; i++) {
        console_putchar(buf[i]);
    }
}

void cpu_list(void) {
    struct {
        uint32_t id;
        uint32_t lapic_id;
        uint64_t switches;
        uint64_t steals;
        uint64_t same_proc_pulls;
        uint64_t ipi_rx;
        int bsp;
        int sched_active;
        int has_current;
        char tname[16];
        enum thread_state tstate;
    } snap[MAX_CPUS];
    uint32_t ncpu = 0;

    spin_lock(&cpu_list_lock);
    ncpu = cpu_count;
    for (uint32_t i = 0; i < ncpu && i < MAX_CPUS; i++) {
        struct cpu *c = &cpus[i];
        snap[i].id = c->id;
        snap[i].lapic_id = c->lapic_id;
        snap[i].switches = c->switches;
        snap[i].steals = c->steals;
        snap[i].same_proc_pulls = c->same_proc_pulls;
        snap[i].ipi_rx = c->ipi_rx;
        snap[i].bsp = c->bsp;
        snap[i].sched_active = c->sched_active;
        if (c->current) {
            snap[i].has_current = 1;
            snap[i].tstate = c->current->state;
            strcpy_snap(snap[i].tname, c->current->name, (int)sizeof(snap[i].tname));
        } else {
            snap[i].has_current = 0;
            snap[i].tname[0] = '\0';
            snap[i].tstate = THREAD_READY;
        }
    }
    spin_unlock(&cpu_list_lock);

    console_writestring("\nCPU  Lapic  BSP  Sched  Switches  Steals  Pulls  IPI-rx  Current thread\n");
    console_writestring("---  -----  ---  -----  ---------  ------  -----  ------  -------------------------\n");

    for (uint32_t i = 0; i < ncpu; i++) {
        write_u64_padded((uint64_t)snap[i].id, 3);
        console_writestring("  ");
        write_u64_padded((uint64_t)snap[i].lapic_id, 5);
        console_writestring("  ");
        write_padded(snap[i].bsp ? "yes" : "no", 3);
        console_writestring("  ");
        write_padded(snap[i].sched_active ? "on" : "off", 5);
        console_writestring("  ");
        write_u64_padded(snap[i].switches, 9);
        console_writestring("  ");
        write_u64_padded(snap[i].steals, 6);
        console_writestring("  ");
        write_u64_padded(snap[i].same_proc_pulls, 5);
        console_writestring("  ");
        write_u64_padded(snap[i].ipi_rx, 6);
        console_writestring("  ");

        if (snap[i].has_current) {
            write_padded(snap[i].tname, 10);
            console_writestring(" (");
            switch (snap[i].tstate) {
                case THREAD_READY: console_writestring("ready"); break;
                case THREAD_RUNNING: console_writestring("running"); break;
                case THREAD_BLOCKED: console_writestring("blocked"); break;
                case THREAD_TERMINATED: console_writestring("dead"); break;
                default: console_writestring("?"); break;
            }
            console_putchar(')');
        } else {
            write_padded("-", 10);
        }
        console_putchar('\n');
    }

    console_writestring("\n");
    console_write_dec(ncpu);
    console_writestring(" CPU(s) online\n");
}
