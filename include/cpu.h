#ifndef CPU_H
#define CPU_H

#include <stdint.h>

#include <limine.h>

#include "lwkt.h"
#include "spinlock.h"

#define MAX_CPUS 8

struct cpu {
    struct cpu *self;
    uint32_t id;
    uint32_t lapic_id;
    int online;
    int bsp;
    struct lwkt_thread *current;
    struct lwkt_thread idle;
    struct lwkt_thread *run_queues[MAX_PRIORITY];
    spinlock_t queue_lock;
    volatile int preempt_requested;
    int sched_active;
    uint64_t bootstrap_rsp;
    uint64_t switches;
    uint64_t steals;
    uint64_t same_proc_pulls;
    uint64_t ipi_rx;
};

struct cpu *cpu_current(void);
struct cpu *cpu_by_id(uint32_t id);
uint32_t cpu_online_count(void);
int cpu_is_bsp(void);
int cpu_index_of_thread(struct lwkt_thread *t);

void cpu_list(void);

void cpu_init_bsp(void);
struct cpu *cpu_init_ap(uint32_t lapic_id);
void cpu_set_gs(struct cpu *cpu);

void smp_init(volatile struct LIMINE_MP(response) *smp);
void smp_release_aps(void);
void smp_start_aps(void);

#endif
