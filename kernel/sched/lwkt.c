#include "lwkt.h"

#include "console.h"
#include "cpu.h"
#include "gdt.h"
#include "interrupt.h"
#include "lapic.h"
#include "msgport.h"
#include "proc.h"
#include "spinlock.h"
#include "uthread.h"
#include "vmm.h"

#include <stddef.h>
#include <stdint.h>

extern void switch_context(uint64_t *old_rsp, uint64_t new_rsp);
extern void thread_bootstrap(void);

static struct lwkt_thread thread_pool[MAX_THREADS];
static spinlock_t sched_lock;
static int thread_count;
static uint32_t next_id = 1;
static int sched_active;

static struct cpu *this_cpu(void);
static void strcpy_local(char *dst, const char *src);

static void cpu_run_queues_init(struct cpu *cpu) {
    if (!cpu) {
        return;
    }
    for (int i = 0; i < MAX_PRIORITY; i++) {
        cpu->run_queues[i] = NULL;
    }
}

static void enqueue_on_cpu(struct cpu *cpu, struct lwkt_thread *t) {
    if (!cpu || !t || t->state == THREAD_TERMINATED) {
        return;
    }

    uint32_t p = t->priority;
    t->next = NULL;
    t->run_cpu = cpu->id;

    if (!cpu->run_queues[p]) {
        cpu->run_queues[p] = t;
        return;
    }

    struct lwkt_thread *tail = cpu->run_queues[p];
    while (tail->next) {
        tail = tail->next;
    }
    tail->next = t;
}

static void enqueue_thread(struct lwkt_thread *t) {
    struct cpu *cpu = this_cpu();
    if (!cpu) {
        cpu = cpu_by_id(0);
    }
    enqueue_on_cpu(cpu, t);
}

static int remove_from_cpu_queues(struct cpu *cpu, struct lwkt_thread *t) {
    if (!cpu || !t) {
        return 0;
    }
    for (int p = 0; p < MAX_PRIORITY; p++) {
        struct lwkt_thread **slot = &cpu->run_queues[p];
        while (*slot) {
            if (*slot == t) {
                *slot = t->next;
                t->next = NULL;
                return 1;
            }
            slot = &(*slot)->next;
        }
    }
    return 0;
}

static int remove_from_queues(struct lwkt_thread *t) {
    int removed = 0;
    for (uint32_t i = 0; i < cpu_online_count(); i++) {
        struct cpu *c = cpu_by_id(i);
        if (c && c->online) {
            removed |= remove_from_cpu_queues(c, t);
        }
    }
    return removed;
}

/*
 * Dequeue one runnable thread from cpu's local queues.
 * Returns NULL if empty (or only skip matches with use_skip_fallback).
 */
static struct lwkt_thread *dequeue_from_cpu(struct cpu *cpu, struct lwkt_thread *skip,
                                          int use_skip_fallback) {
    struct lwkt_thread *fallback = NULL;

    for (uint32_t p = 0; p < MAX_PRIORITY; p++) {
        struct lwkt_thread **slot = &cpu->run_queues[p];
        while (*slot) {
            struct lwkt_thread *t = *slot;
            if (t->state == THREAD_TERMINATED) {
                *slot = t->next;
                t->next = NULL;
                continue;
            }

            if (t == skip) {
                fallback = t;
                slot = &t->next;
                continue;
            }

            *slot = t->next;
            t->next = NULL;
            return t;
        }
    }

    if (use_skip_fallback && fallback) {
        remove_from_queues(fallback);
        return fallback;
    }

    return NULL;
}

static struct lwkt_thread *dequeue_thread(struct cpu *cpu, struct lwkt_thread *skip) {
    struct lwkt_thread *t = dequeue_from_cpu(cpu, skip, 1);
    if (t) {
        return t;
    }

    /* Work steal: idle CPU pulls from another CPU's queue. */
    for (uint32_t i = 0; i < cpu_online_count(); i++) {
        struct cpu *other = cpu_by_id(i);
        if (!other || !other->online || other == cpu) {
            continue;
        }
        t = dequeue_from_cpu(other, NULL, 0);
        if (t) {
            t->run_cpu = cpu->id;
            return t;
        }
    }

    return &cpu->idle;
}

static int cpu_has_runnable(struct cpu *cpu) {
    if (!cpu) {
        return 0;
    }
    for (uint32_t p = 0; p < MAX_PRIORITY; p++) {
        if (cpu->run_queues[p]) {
            return 1;
        }
    }
    return 0;
}

static void strcpy_local(char *dst, const char *src) {
    while (*src) {
        *dst++ = *src++;
    }
    *dst = '\0';
}

static struct cpu *this_cpu(void) {
    return cpu_current();
}

static void init_thread_stack(struct lwkt_thread *t) {
    uint64_t *sp = (uint64_t *)(t->stack + STACK_SIZE);
    sp = (uint64_t *)((uint64_t)sp & ~0xFULL);

    *--sp = (uint64_t)t->entry_point;
    *--sp = (uint64_t)t->arg;
    *--sp = (uint64_t)thread_bootstrap;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;

    t->rsp = (uint64_t)sp;
}

static struct lwkt_thread *find_thread(uint32_t id) {
    for (int i = 0; i < MAX_THREADS; i++) {
        if (thread_pool[i].id == id) {
            return &thread_pool[i];
        }
    }
    return NULL;
}

static const char *state_name(enum thread_state state) {
    switch (state) {
        case THREAD_READY: return "ready";
        case THREAD_RUNNING: return "running";
        case THREAD_BLOCKED: return "blocked";
        case THREAD_TERMINATED: return "dead";
        default: return "?";
    }
}

static void print_uthread_cols(struct lwkt_thread *t) {
    struct uthread *u = t ? t->uthread : NULL;
    if (!u) {
        console_writestring("  -   -  ");
        return;
    }
    console_writestring("  ");
    console_write_dec(uthread_slot_of(u));
    console_writestring("   ");
    int pin = uthread_index_in_proc(u);
    if (pin > 0) {
        console_write_dec((uint64_t)pin);
    } else {
        console_putchar('-');
    }
    console_writestring("  ");
}

static void print_thread_row(struct lwkt_thread *t, int is_current) {
    int cpu_idx = cpu_index_of_thread(t);
    console_write_dec(t->id);
    print_uthread_cols(t);
    console_writestring(t->name);
    console_writestring("  ");
    console_write_dec(t->priority);
    console_writestring("     ");
    console_writestring(state_name(t->state));
    if (is_current) {
        console_writestring(" *  ");
    } else {
        console_writestring("    ");
    }
    if (cpu_idx >= 0) {
        console_write_dec((uint64_t)cpu_idx);
    } else {
        console_putchar('-');
    }
    console_writestring("  ");
    if (t->user_cr3) {
        console_write_hex(t->user_cr3);
    } else {
        console_writestring("-");
    }
    console_putchar('\n');
}

static void worker_entry(void *arg) {
    (void)arg;
    struct lwkt_thread *self = lwkt_curthread();
    while (self && self->state != THREAD_TERMINATED) {
        lwkt_yield();
        __asm__ volatile("hlt");
    }
    lwkt_thread_exit();
}

static int has_runnable_threads(void) {
    for (uint32_t i = 0; i < cpu_online_count(); i++) {
        struct cpu *c = cpu_by_id(i);
        if (c && c->online && cpu_has_runnable(c)) {
            return 1;
        }
    }
    return 0;
}

static void idle_worker(void *arg) {
    (void)arg;
    for (;;) {
        lwkt_preempt_check();
        if (has_runnable_threads()) {
            lwkt_yield();
            continue;
        }
        __asm__ volatile("sti; hlt" ::: "memory");
    }
}

void lwkt_default_worker(void *arg) {
    worker_entry(arg);
}

void lwkt_cpu_init_idle(void) {
    struct cpu *cpu = this_cpu();
    if (!cpu) {
        return;
    }

    struct lwkt_thread *idle = &cpu->idle;
    if (cpu->bsp) {
        strcpy_local(idle->name, "idle");
    } else {
        idle->name[0] = 'i';
        idle->name[1] = 'd';
        idle->name[2] = '0' + (char)(cpu->id % 10);
        idle->name[3] = '\0';
    }
    idle->id = 0;
    idle->priority = LWKT_PRIO_LOW;
    idle->state = THREAD_RUNNING;
    idle->entry_point = idle_worker;
    idle->arg = NULL;
    idle->next = NULL;
    idle->run_cpu = cpu->id;
    idle->yields = 0;
    idle->saved_kernel_rsp = 0;
    idle->mbox_slot = 0;
    idle->wait_next = NULL;
    idle->mbox_wait_next = NULL;
    idle->uthread = NULL;
    idle->user_cr3 = 0;
    init_thread_stack(idle);
    cpu_run_queues_init(cpu);
    cpu->current = idle;
}

void lwkt_init(void) {
    spin_init(&sched_lock);
    for (int i = 0; i < MAX_THREADS; i++) {
        thread_pool[i].id = 0;
        thread_pool[i].state = THREAD_TERMINATED;
    }

    lwkt_cpu_init_idle();
    cpu_run_queues_init(this_cpu());
    thread_count = 0;
    sched_active = 0;

    msgport_init();
}

void lwkt_sched_start(void) {
    /* Timer and sched_active are enabled in smp_release_aps() right before bootstrap. */
}

void lwkt_sched_enable(void) {
    struct cpu *cpu = this_cpu();
    if (!sched_active) {
        sched_active = 1;
    }
    if (cpu) {
        cpu->sched_active = 1;
    }
}

void lwkt_sched_ipi_others(void) {
    if (cpu_online_count() > 1) {
        lapic_ipi_reschedule_others();
    }
}

void lwkt_sched_stop(void) {
    timer_set_enabled(0);
    lapic_timer_stop();
    sched_active = 0;
    struct cpu *cpu = this_cpu();
    if (cpu) {
        cpu->sched_active = 0;
    }
}

static void sched_lock_irqsave(uint64_t *irqf) {
    spin_lock_irqsave(&sched_lock, irqf);
}

static void sched_unlock_irqrestore(uint64_t irqf) {
    spin_unlock_irqrestore(&sched_lock, irqf);
}

struct lwkt_thread *lwkt_create(const char *name, void (*entry)(void *), void *arg, uint32_t priority) {
    if (priority >= MAX_PRIORITY) {
        priority = MAX_PRIORITY - 1;
    }

    uint64_t irqf;
    sched_lock_irqsave(&irqf);
    if (thread_count >= MAX_THREADS) {
        sched_unlock_irqrestore(irqf);
        return NULL;
    }

    struct lwkt_thread *t = NULL;
    for (int i = 0; i < MAX_THREADS; i++) {
        if (thread_pool[i].id == 0) {
            t = &thread_pool[i];
            break;
        }
    }
    if (!t) {
        sched_unlock_irqrestore(irqf);
        return NULL;
    }

    if (!entry) {
        entry = worker_entry;
    }
    if (!name || name[0] == '\0') {
        char auto_name[16];
        auto_name[0] = 't';
        auto_name[1] = '0' + (char)(next_id % 10);
        auto_name[2] = '\0';
        name = auto_name;
    }

    t->id = next_id++;
    strcpy_local(t->name, name);
    t->priority = priority;
    t->state = THREAD_READY;
    t->entry_point = entry;
    t->arg = arg;
    t->uthread = NULL;
    t->next = NULL;
    t->run_cpu = 0;
    t->user_cr3 = 0;
    t->pending_cr3_destroy = 0;
    t->yields = 0;
    t->saved_kernel_rsp = 0;
    t->wait_next = NULL;
    t->mbox_wait_next = NULL;
    t->mbox_slot = (uint8_t)((t - thread_pool) + 1);
    init_thread_stack(t);
    enqueue_thread(t);
    thread_count++;
    sched_unlock_irqrestore(irqf);

    lwkt_sched_ipi_others();
    return t;
}

int lwkt_destroy(uint32_t id) {
    if (id == 0) {
        return -1;
    }

    uint64_t irqf;
    sched_lock_irqsave(&irqf);
    struct lwkt_thread *t = find_thread(id);
    if (!t) {
        sched_unlock_irqrestore(irqf);
        return -1;
    }

    enum thread_state old = t->state;
    t->entry_point = NULL;
    remove_from_queues(t);

    if (old == THREAD_BLOCKED) {
        msgport_wakeup(t);
    } else {
        msgport_clear_slot(t->mbox_slot);
    }

    t->state = THREAD_TERMINATED;

    struct cpu *cpu = this_cpu();
    if (t != (cpu ? cpu->current : NULL)) {
        msgport_unregister_id(t->id);
        t->rsp = 0;
        t->id = 0;
        thread_count--;
    }
    sched_unlock_irqrestore(irqf);
    return 0;
}

struct lwkt_thread *lwkt_find(uint32_t id) {
    if (id == 0) {
        struct cpu *cpu = this_cpu();
        return cpu ? &cpu->idle : NULL;
    }
    return find_thread(id);
}

void lwkt_info(uint32_t id) {
    struct lwkt_thread *t = lwkt_find(id);
    if (!t || (id != 0 && t->id == 0)) {
        console_writestring("\nNo such thread\n");
        return;
    }

    console_writestring("\nThread ");
    console_write_dec(t->id);
    console_writestring("\n  Name:     ");
    console_writestring(t->name);
    console_writestring("\n  Priority: ");
    console_write_dec(t->priority);
    console_writestring(" (0=highest)\n  State:    ");
    console_writestring(state_name(t->state));
    if (t == lwkt_curthread()) {
        console_writestring(" (current)");
    }
    console_writestring("\n  Yields:   ");
    console_write_dec(t->yields);
    if (t->uthread) {
        console_writestring("\n  Uthr slot:");
        console_write_dec(uthread_slot_of(t->uthread));
        console_writestring("  in-proc: ");
        int pin = uthread_index_in_proc(t->uthread);
        if (pin > 0) {
            console_write_dec((uint64_t)pin);
        } else {
            console_putchar('-');
        }
        if (t->uthread->proc && t->uthread->proc->pid) {
            console_writestring("  PID: ");
            console_write_dec(t->uthread->proc->pid);
        }
    } else {
        console_writestring("\n  Uthread:  (none)");
    }
    console_writestring("\n  RSP:      0x");
    console_write_hex(t->rsp);
    console_putchar('\n');
}

void lwkt_list(void) {
    struct cpu *cpu = this_cpu();
    struct lwkt_thread *current = cpu ? cpu->current : NULL;

    console_writestring("\nLWKT  Slot Proc  Name            Prio  State      CPU CR3\n");
    console_writestring("----  ---- ----  --------------  ----  ---------  --- ---------------\n");

    if (current) {
        print_thread_row(current, 1);
    }

    uint64_t irqf;
    sched_lock_irqsave(&irqf);
    for (uint32_t ci = 0; ci < cpu_online_count(); ci++) {
        struct cpu *c = cpu_by_id(ci);
        if (!c || !c->online) {
            continue;
        }
        for (int p = 0; p < MAX_PRIORITY; p++) {
            for (struct lwkt_thread *t = c->run_queues[p]; t; t = t->next) {
                if (t == current || t->state == THREAD_TERMINATED) {
                    continue;
                }
                print_thread_row(t, 0);
            }
        }
    }

    for (int i = 0; i < MAX_THREADS; i++) {
        struct lwkt_thread *t = &thread_pool[i];
        if (t->id == 0 || t == current || t->state != THREAD_BLOCKED) {
            continue;
        }
        print_thread_row(t, 0);
    }
    sched_unlock_irqrestore(irqf);

    console_writestring("\nTotal: ");
    console_write_dec((uint64_t)thread_count + cpu_online_count());
    console_writestring(" threads (incl. idle per CPU)\n");
}

struct lwkt_thread *lwkt_curthread(void) {
    struct cpu *cpu = this_cpu();
    return cpu ? cpu->current : NULL;
}

int lwkt_thread_count(void) {
    return thread_count;
}

static void lwkt_apply_cr3(struct lwkt_thread *t) {
    uint64_t cr3 = t && t->user_cr3 ? t->user_cr3 : vmm_kernel_cr3();
    vmm_switch(cr3);
}

static void lwkt_apply_tss(struct lwkt_thread *t) {
    if (!t || !t->uthread || t->uthread->type != UTHREAD_USER) {
        return;
    }
    uint64_t top = (uint64_t)(uintptr_t)t->stack + STACK_SIZE;
    tss_set_rsp0(top & ~0xFULL);
}

void lwkt_bootstrap_first(void) {
    struct cpu *cpu = this_cpu();
    uint64_t irqf;

    sched_lock_irqsave(&irqf);
    struct lwkt_thread *next = dequeue_thread(cpu, NULL);
    if (!next) {
        next = &cpu->idle;
    }

    cpu->idle.state = THREAD_READY;
    cpu->current = next;
    next->state = THREAD_RUNNING;
    sched_unlock_irqrestore(irqf);

    /* Shell is RUNNING on BSP (not in queue); safe to wake AP schedulers. */
    smp_start_aps();

    static uint64_t boot_saved_rsp;
    lwkt_apply_cr3(next);
    lwkt_apply_tss(next);
    switch_context(&boot_saved_rsp, next->rsp);
    __builtin_unreachable();
}

void lwkt_ap_bootstrap(void) {
    struct cpu *cpu = this_cpu();
    if (!cpu) {
        for (;;) {
            __asm__ volatile("hlt");
        }
    }

    lwkt_apply_cr3(&cpu->idle);
    switch_context(&cpu->bootstrap_rsp, cpu->idle.rsp);
    __builtin_unreachable();
}

void lwkt_preempt_request(void) {
    struct cpu *cpu = this_cpu();
    if (cpu && cpu->sched_active) {
        cpu->preempt_requested = 1;
    }
}

void lwkt_preempt_check(void) {
    struct cpu *cpu = this_cpu();
    if (!cpu || !cpu->preempt_requested || !cpu->sched_active) {
        return;
    }

    struct lwkt_thread *cur = cpu->current;
    if (!cur) {
        cpu->preempt_requested = 0;
        return;
    }

    int other = 0;
    if (!spin_trylock(&sched_lock)) {
        return;
    }
    for (uint32_t ci = 0; ci < cpu_online_count(); ci++) {
        struct cpu *c = cpu_by_id(ci);
        if (!c || !c->online) {
            continue;
        }
        for (uint32_t p = 0; p < MAX_PRIORITY; p++) {
            for (struct lwkt_thread *t = c->run_queues[p]; t; t = t->next) {
                if (t != cur && t->state != THREAD_TERMINATED) {
                    other = 1;
                    break;
                }
            }
            if (other) {
                break;
            }
        }
        if (other) {
            break;
        }
    }
    spin_unlock(&sched_lock);

    if (!other && cur == &cpu->idle) {
        return;
    }

    cpu->preempt_requested = 0;
    lwkt_switch();
}

void lwkt_switch(void) {
    struct cpu *cpu = this_cpu();
    struct lwkt_thread *prev = cpu->current;
    uint64_t irqf = cpu_irq_save();

    spin_lock(&sched_lock);
    if (prev && prev->state == THREAD_RUNNING) {
        prev->state = THREAD_READY;
        prev->yields++;
        enqueue_thread(prev);
    }

    struct lwkt_thread *next = dequeue_thread(cpu, prev);
    if (next == prev) {
        next->state = THREAD_RUNNING;
        spin_unlock(&sched_lock);
        cpu_irq_restore(irqf);
        return;
    }

    if (next->state == THREAD_TERMINATED) {
        next = &cpu->idle;
    }

    next->state = THREAD_RUNNING;
    cpu->current = next;
    spin_unlock(&sched_lock);

    if (next->rsp == 0) {
        next = &cpu->idle;
        next->state = THREAD_RUNNING;
        cpu->current = next;
        if (next->rsp == 0) {
            cpu_irq_restore(irqf);
            return;
        }
    }

    lwkt_apply_cr3(next);
    lwkt_apply_tss(next);

    if (prev && prev->pending_cr3_destroy) {
        uint64_t destroy_cr3 = prev->pending_cr3_destroy;
        prev->pending_cr3_destroy = 0;
        vmm_aspace_destroy(destroy_cr3);
    }

    /*
     * If we are on the interrupt/syscall stack (outside the LWKT stack array),
     * do not let switch_context overwrite prev->rsp with that pointer.
     */
    if (prev && prev->uthread) {
        uint64_t hw_rsp;
        uint64_t lo = (uint64_t)(uintptr_t)prev->stack;
        uint64_t hi = lo + STACK_SIZE;
        __asm__ volatile("mov %%rsp, %0" : "=r"(hw_rsp));
        if (hw_rsp < lo || hw_rsp >= hi) {
            __asm__ volatile("mov %0, %%rsp" :: "r"(prev->rsp) : "memory");
        }
    }

    cpu->switches++;
    switch_context(&prev->rsp, next->rsp);
    cpu_irq_restore(irqf);
}

void lwkt_yield(void) {
    lwkt_switch();
}

void lwkt_block(void) {
    struct cpu *cpu = this_cpu();
    struct lwkt_thread *cur = cpu ? cpu->current : NULL;
    if (!cur || cur->state != THREAD_RUNNING) {
        return;
    }
    cur->state = THREAD_BLOCKED;
    lwkt_switch();
}

void lwkt_unblock(struct lwkt_thread *t) {
    if (!t || t->state != THREAD_BLOCKED) {
        return;
    }
    uint64_t irqf;
    sched_lock_irqsave(&irqf);
    t->state = THREAD_READY;
    enqueue_thread(t);
    sched_unlock_irqrestore(irqf);
    lwkt_sched_ipi_others();
}

void lwkt_thread_exit(void) {
    struct cpu *cpu = this_cpu();
    if (cpu && cpu->current) {
        uint64_t irqf;
        sched_lock_irqsave(&irqf);
        remove_from_queues(cpu->current);
        sched_unlock_irqrestore(irqf);
        msgport_clear_slot(cpu->current->mbox_slot);
        msgport_unregister_id(cpu->current->id);
        cpu->current->state = THREAD_TERMINATED;
        cpu->current->entry_point = NULL;
        cpu->current->id = 0;
        thread_count--;
    }
    lwkt_yield();
    for (;;) {
        __asm__ volatile("hlt");
    }
}
