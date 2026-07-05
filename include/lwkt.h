#ifndef LWKT_H
#define LWKT_H

#include <stdint.h>

struct uthread;

#define MAX_THREADS 32
#define MAX_PRIORITY 16
#define STACK_SIZE 4096

#define LWKT_PRIO_SHELL    0
#define LWKT_PRIO_HIGH     2
#define LWKT_PRIO_NORMAL   8
#define LWKT_PRIO_LOW      15

enum thread_state {
    THREAD_READY = 0,
    THREAD_RUNNING = 1,
    THREAD_BLOCKED = 2,
    THREAD_TERMINATED = 3
};

struct lwkt_thread {
    uint32_t id;
    char name[32];
    uint32_t priority;
    enum thread_state state;
    void (*entry_point)(void *arg);
    void *arg;
    struct uthread *uthread;
    struct lwkt_thread *next;
    uint32_t run_cpu;
    uint64_t rsp;
    uint64_t user_cr3;
    uint64_t pending_cr3_destroy;
    uint64_t yields;
    uint64_t saved_kernel_rsp;
    uint8_t mbox_slot;
    struct lwkt_thread *wait_next;      /* token / proc_mutex wait queues */
    struct lwkt_thread *mbox_wait_next; /* msgport read_waiters only */
    uint8_t stack[STACK_SIZE];
};

void lwkt_init(void);
void lwkt_sched_start(void);
void lwkt_sched_stop(void);
void lwkt_sched_enable(void);
void lwkt_sched_ipi_others(void);

struct lwkt_thread *lwkt_create(const char *name, void (*entry)(void *), void *arg, uint32_t priority);
int lwkt_destroy(uint32_t id);
struct lwkt_thread *lwkt_find(uint32_t id);
void lwkt_list(void);
void lwkt_info(uint32_t id);

void lwkt_switch(void);
void lwkt_yield(void);
void lwkt_block(void);
void lwkt_unblock(struct lwkt_thread *t);
void lwkt_preempt_request(void);
void lwkt_preempt_check(void);
void lwkt_thread_exit(void);

void lwkt_cpu_init_idle(void);
void lwkt_ap_bootstrap(void);

void lwkt_default_worker(void *arg);

struct lwkt_thread *lwkt_curthread(void);
int lwkt_thread_count(void);
void lwkt_bootstrap_first(void);

#endif
