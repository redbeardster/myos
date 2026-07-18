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
static spinlock_t thread_pool_lock;
static int thread_count;
static uint32_t next_id = 1;
static int sched_active;
static int ipc_bump_enabled = 1;
static uint64_t ipc_bump_attempts;
static uint64_t ipc_bump_applied;
static uint64_t ipc_bump_skipped_debounce;
static uint64_t ipc_bump_skipped_disabled;
static uint64_t ipi_targeted;
static uint64_t ipi_local;
static uint64_t ipi_broadcast;

static struct cpu *this_cpu(void);
static void strcpy_local(char *dst, const char *src);
static int thread_owner_cpu_id(struct lwkt_thread *t);

/* Per-CPU idle must never sit on a run queue (steal would migrate it). */
static int is_idle_thread(const struct lwkt_thread *t) {
    if (!t) {
        return 0;
    }
    for (uint32_t i = 0; i < cpu_online_count(); i++) {
        struct cpu *c = cpu_by_id(i);
        if (c && t == &c->idle) {
            return 1;
        }
    }
    return 0;
}

/* Foreign steal/pull: READY only, not idle, not mid-switch (pinned or still current). */
static int stealable_foreign(struct lwkt_thread *t) {
    if (!t || t->state != THREAD_READY || t->queue_pinned || is_idle_thread(t)) {
        return 0;
    }
    return thread_owner_cpu_id(t) < 0;
}

static struct cpu *cpu_for_thread_wake(struct lwkt_thread *t) {
    int owner = thread_owner_cpu_id(t);
    struct cpu *dest = NULL;
    if (owner >= 0) {
        dest = cpu_by_id((uint32_t)owner);
    }
    if ((!dest || !dest->online) && t) {
        dest = cpu_by_id(t->run_cpu);
    }
    if (!dest || !dest->online) {
        dest = this_cpu();
    }
    return dest;
}

static void cpu_run_queues_init(struct cpu *cpu) {
    if (!cpu) {
        return;
    }
    for (int i = 0; i < MAX_PRIORITY; i++) {
        cpu->run_queues[i] = NULL;
    }
}

static void queue_lock(struct cpu *c) {
    if (c) {
        spin_lock(&c->queue_lock);
    }
}

static void queue_unlock(struct cpu *c) {
    if (c) {
        spin_unlock(&c->queue_lock);
    }
}

static void queue_lock_two(struct cpu *a, struct cpu *b) {
    if (!a) {
        queue_lock(b);
        return;
    }
    if (!b || a == b) {
        queue_lock(a);
        return;
    }
    if (a->id < b->id) {
        queue_lock(a);
        queue_lock(b);
    } else {
        queue_lock(b);
        queue_lock(a);
    }
}

static void queue_unlock_two(struct cpu *a, struct cpu *b) {
    if (!a) {
        queue_unlock(b);
        return;
    }
    if (!b || a == b) {
        queue_unlock(a);
        return;
    }
    if (a->id < b->id) {
        queue_unlock(b);
        queue_unlock(a);
    } else {
        queue_unlock(a);
        queue_unlock(b);
    }
}

static void pool_lock_irqsave(uint64_t *irqf) {
    spin_lock_irqsave(&thread_pool_lock, irqf);
}

static void pool_unlock_irqrestore(uint64_t irqf) {
    spin_unlock_irqrestore(&thread_pool_lock, irqf);
}

static void enqueue_on_cpu(struct cpu *cpu, struct lwkt_thread *t) {
    if (!cpu || !t || t->state == THREAD_TERMINATED || is_idle_thread(t)) {
        return;
    }

    uint32_t p = t->priority;
    t->next = NULL;
    t->run_cpu = cpu->id;

    struct lwkt_thread **slot = &cpu->run_queues[p];
    while (*slot) {
        slot = &(*slot)->next;
    }
    *slot = t;
}

static void enqueue_on_cpu_head(struct cpu *cpu, struct lwkt_thread *t) {
    if (!cpu || !t || t->state == THREAD_TERMINATED || is_idle_thread(t)) {
        return;
    }

    uint32_t p = t->priority;
    t->run_cpu = cpu->id;
    t->next = cpu->run_queues[p];
    cpu->run_queues[p] = t;
}

static struct cpu *pick_enqueue_cpu(struct uthread *bind_u) {
    struct cpu *cpu = this_cpu();
    if (!cpu) {
        return cpu_by_id(0);
    }

    struct lwkt_thread *cur = lwkt_curthread();
    if (bind_u && bind_u->type == UTHREAD_USER && cur && cur->uthread && cur->in_syscall &&
        bind_u->proc && cur->uthread->proc == bind_u->proc) {
        return cpu;
    }

    if (cur && cur->in_syscall) {
        uint32_t n = cpu_online_count();
        for (uint32_t i = 1; i < n; i++) {
            struct cpu *other = cpu_by_id((cpu->id + i) % n);
            if (other && other->online) {
                return other;
            }
        }
    }
    return cpu;
}

/* Returns destination CPU after enqueue (locks released). IRQs safe. */
static struct cpu *enqueue_thread(struct lwkt_thread *t) {
    struct uthread *bind_u = t ? t->uthread : NULL;
    struct cpu *cpu = pick_enqueue_cpu(bind_u);
    if (!cpu) {
        cpu = cpu_by_id(0);
    }
    uint64_t irqf = cpu_irq_save();
    queue_lock(cpu);
    enqueue_on_cpu(cpu, t);
    queue_unlock(cpu);
    cpu_irq_restore(irqf);
    return cpu;
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

/* IRQs safe. */
static int remove_from_queues(struct lwkt_thread *t) {
    if (!t) {
        return 0;
    }

    uint64_t irqf = cpu_irq_save();
    struct cpu *prefer = cpu_by_id(t->run_cpu);
    if (prefer && prefer->online) {
        queue_lock(prefer);
        if (remove_from_cpu_queues(prefer, t)) {
            queue_unlock(prefer);
            cpu_irq_restore(irqf);
            return 1;
        }
        queue_unlock(prefer);
    }

    for (uint32_t i = 0; i < cpu_online_count(); i++) {
        struct cpu *c = cpu_by_id(i);
        if (!c || !c->online || c == prefer) {
            continue;
        }
        queue_lock(c);
        if (remove_from_cpu_queues(c, t)) {
            queue_unlock(c);
            cpu_irq_restore(irqf);
            return 1;
        }
        queue_unlock(c);
    }
    cpu_irq_restore(irqf);
    return 0;
}

/*
 * Dequeue one runnable thread from cpu's local queues.
 * Caller must hold cpu->queue_lock.
 * foreign=1: only stealable_foreign threads (steal / same-proc pull).
 */
static struct lwkt_thread *dequeue_from_cpu(struct cpu *cpu, struct lwkt_thread *skip,
                                          int use_skip_fallback, int foreign) {
    struct lwkt_thread *fallback = NULL;

    for (uint32_t p = 0; p < MAX_PRIORITY; p++) {
        struct lwkt_thread **slot = &cpu->run_queues[p];
        while (*slot) {
            struct lwkt_thread *t = *slot;
            if (t->state == THREAD_TERMINATED || is_idle_thread(t)) {
                *slot = t->next;
                t->next = NULL;
                continue;
            }

            if (foreign && !stealable_foreign(t)) {
                slot = &t->next;
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
        remove_from_cpu_queues(cpu, fallback);
        return fallback;
    }

    return NULL;
}

/* Count foreign-stealable threads on cpu (caller holds queue_lock). */
static int count_stealable_foreign(struct cpu *cpu) {
    int n = 0;
    for (uint32_t p = 0; p < MAX_PRIORITY; p++) {
        for (struct lwkt_thread *t = cpu->run_queues[p]; t; t = t->next) {
            if (stealable_foreign(t)) {
                n++;
            }
        }
    }
    return n;
}

/* Acquires/releases queue locks; IRQs should already be off. */
static struct lwkt_thread *dequeue_thread(struct cpu *cpu, struct lwkt_thread *skip) {
    int skip_fallback = 1;
    if (skip && skip->quantum_force) {
        skip_fallback = 0;
    }

    queue_lock(cpu);
    struct lwkt_thread *t = dequeue_from_cpu(cpu, skip, 0, 0);
    if (t) {
        if (skip && skip->quantum_force && t != skip) {
            skip->quantum_force = 0;
        }
        /* Claim under lock: lwkt_nudge must not see READY and re-enqueue. */
        t->state = THREAD_RUNNING;
        queue_unlock(cpu);
        return t;
    }

    if (skip && skip->quantum_force) {
        skip->quantum_force = 0;
    }

    /*
     * skip_fallback on a CPU that only has the yielding thread lets it spin
     * while sibling uthreads sit on other CPUs — pull same-proc peers first.
     * Never hold one queue_lock then take another: unlock, then lock_two.
     */
    if (skip && skip->uthread && skip->uthread->proc) {
        struct proc *proc = skip->uthread->proc;
        queue_unlock(cpu);
        for (uint32_t i = 0; i < cpu_online_count(); i++) {
            struct cpu *other = cpu_by_id(i);
            if (!other || !other->online || other == cpu) {
                continue;
            }
            queue_lock_two(cpu, other);
            /* Prefer any local arrival while unlocked. */
            t = dequeue_from_cpu(cpu, skip, 0, 0);
            if (t) {
                t->state = THREAD_RUNNING;
                queue_unlock_two(cpu, other);
                return t;
            }
            t = dequeue_from_cpu(other, NULL, 0, 1);
            if (!t) {
                queue_unlock_two(cpu, other);
                continue;
            }
            if (t->uthread && t->uthread->proc == proc) {
                t->run_cpu = cpu->id;
                t->state = THREAD_RUNNING;
                cpu->same_proc_pulls++;
                queue_unlock_two(cpu, other);
                return t;
            }
            enqueue_on_cpu(other, t);
            queue_unlock_two(cpu, other);
        }
        queue_lock(cpu);
    }

    if (skip_fallback) {
        t = dequeue_from_cpu(cpu, skip, 1, 0);
        if (t) {
            t->state = THREAD_RUNNING;
            queue_unlock(cpu);
            return t;
        }
    }
    queue_unlock(cpu);

    /* Work steal: lock ids ascending via queue_lock_two. */
    for (uint32_t i = 0; i < cpu_online_count(); i++) {
        struct cpu *other = cpu_by_id(i);
        if (!other || !other->online || other == cpu) {
            continue;
        }
        queue_lock_two(cpu, other);
        t = dequeue_from_cpu(cpu, skip, skip_fallback, 0);
        if (t) {
            t->state = THREAD_RUNNING;
            queue_unlock_two(cpu, other);
            return t;
        }
        /*
         * Steal from idle always; from busy only if ≥2 stealable waiters so we
         * never take the sole queued peer of a mid-switch / running CPU.
         */
        {
            int idle_victim = (other->current == &other->idle);
            int n = count_stealable_foreign(other);
            if (!idle_victim && n < 2) {
                queue_unlock_two(cpu, other);
                continue;
            }
        }
        t = dequeue_from_cpu(other, NULL, 0, 1);
        if (t) {
            t->run_cpu = cpu->id;
            t->state = THREAD_RUNNING;
            cpu->steals++;
            queue_unlock_two(cpu, other);
            return t;
        }
        queue_unlock_two(cpu, other);
    }

    return &cpu->idle;
}

static int cpu_has_ready_peer(struct cpu *cpu) {
    if (!cpu) {
        return 0;
    }
    for (uint32_t p = 0; p < MAX_PRIORITY; p++) {
        int guard = 0;
        for (struct lwkt_thread *t = cpu->run_queues[p]; t && guard < MAX_THREADS;
             t = t->next, guard++) {
            if (t == &cpu->idle) {
                continue;
            }
            if (t->state == THREAD_READY) {
                return 1;
            }
        }
    }
    return 0;
}

static int any_cpu_has_ready_user_runner(void) {
    uint64_t irqf = cpu_irq_save();
    for (uint32_t i = 0; i < cpu_online_count(); i++) {
        struct cpu *c = cpu_by_id(i);
        if (!c || !c->online) {
            continue;
        }
        if (!spin_trylock(&c->queue_lock)) {
            cpu_irq_restore(irqf);
            return 1;
        }
        int found = 0;
        for (uint32_t p = 0; p < MAX_PRIORITY; p++) {
            int guard = 0;
            for (struct lwkt_thread *t = c->run_queues[p]; t && guard < MAX_THREADS;
                 t = t->next, guard++) {
                if (t == &c->idle || !t->user_proc) {
                    continue;
                }
                if (t->state == THREAD_READY) {
                    found = 1;
                    break;
                }
            }
            if (found) {
                break;
            }
        }
        spin_unlock(&c->queue_lock);
        if (found) {
            cpu_irq_restore(irqf);
            return 1;
        }
    }
    cpu_irq_restore(irqf);
    return 0;
}

static int idle_should_yield(struct cpu *cpu) {
    if (!cpu) {
        return 0;
    }
    uint64_t irqf = cpu_irq_save();
    if (!spin_trylock(&cpu->queue_lock)) {
        cpu_irq_restore(irqf);
        return 1;
    }
    int wake = cpu_has_ready_peer(cpu);
    spin_unlock(&cpu->queue_lock);
    cpu_irq_restore(irqf);
    if (wake) {
        return 1;
    }
    return any_cpu_has_ready_user_runner();
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

static int thread_owner_cpu_id(struct lwkt_thread *t) {
    if (!t) {
        return -1;
    }
    for (uint32_t i = 0; i < cpu_online_count(); i++) {
        struct cpu *c = cpu_by_id(i);
        if (!c || !c->online) {
            continue;
        }
        if (c->current == t) {
            return (int)c->id;
        }
    }
    return -1;
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
    struct uthread *u = NULL;
    if (t && t->user_proc) {
        u = t->user_proc->current_uthread;
    } else if (t) {
        u = t->uthread;
    }
    if (!u) {
        console_writestring("  -   -  ");
        return;
    }
    console_writestring("  ");
    console_write_dec(uthread_slot_of(u));
    console_writestring("   ");
    if (u->proc && u->proc->pid) {
        console_write_dec(u->proc->pid);
    } else {
        console_putchar('-');
    }
    console_writestring("  ");
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

static void write_hex_padded(uint64_t v, int width) {
    char buf[24];
    int n = 0;
    if (v == 0) {
        buf[n++] = '-';
    } else {
        static const char hex[] = "0123456789abcdef";
        char tmp[16];
        int t = 0;
        while (v > 0 && t < (int)sizeof(tmp)) {
            tmp[t++] = hex[v & 0xFULL];
            v >>= 4;
        }
        buf[n++] = '0';
        buf[n++] = 'x';
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

static void write_ratio_padded(uint64_t a, uint64_t b, int width) {
    char buf[32];
    int n = 0;
    char tmp[20];
    int t = 0;

    if (a == 0) {
        buf[n++] = '0';
    } else {
        t = 0;
        while (a > 0 && t < (int)sizeof(tmp)) {
            tmp[t++] = (char)('0' + (a % 10));
            a /= 10;
        }
        while (t > 0) {
            buf[n++] = tmp[--t];
        }
    }
    buf[n++] = '/';
    if (b == 0) {
        buf[n++] = '0';
    } else {
        t = 0;
        while (b > 0 && t < (int)sizeof(tmp)) {
            tmp[t++] = (char)('0' + (b % 10));
            b /= 10;
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

static void print_thread_row(struct lwkt_thread *t, int is_current) {
    int cpu_idx = cpu_index_of_thread(t);
    write_u64_padded(t->id, 4);
    print_uthread_cols(t);
    write_padded(t->name, 14);
    console_writestring("  ");
    write_u64_padded(t->priority, 4);
    console_writestring("  ");
    write_padded(state_name(t->state), 9);
    if (is_current) {
        console_writestring("* ");
    } else {
        console_writestring("  ");
    }
    if (cpu_idx >= 0) {
        write_u64_padded((uint64_t)cpu_idx, 3);
    } else {
        write_padded("-", 3);
    }
    console_writestring("  ");
    write_hex_padded(t->user_cr3, 15);
    console_writestring("  ");
    console_write_dec(t->quantum_expired);
    console_writestring("/");
    console_write_dec(t->quantum_forced_switches);
    console_writestring("/");
    console_write_dec(t->cpu_migrations);
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

static void idle_worker(void *arg) {
    (void)arg;
    for (;;) {
        struct cpu *cpu = this_cpu();
        lwkt_preempt_check();
        if (idle_should_yield(cpu)) {
            lwkt_yield();
            continue;
        }
        /*
         * Classic idle race: clearing preempt_requested then sti;hlt loses a
         * wake that arrives after the clear. Enable IRQs, recheck, then hlt.
         */
        __asm__ volatile("sti" ::: "memory");
        if (cpu->preempt_requested || idle_should_yield(cpu)) {
            continue;
        }
        __asm__ volatile("hlt" ::: "memory");
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
    idle->quantum_expired = 0;
    idle->quantum_forced_switches = 0;
    idle->saved_kernel_rsp = 0;
    idle->mbox_slot = 0;
    idle->pending_kill = 0;
    idle->quantum_left = LWKT_QUANTUM_TICKS;
    idle->quantum_force = 0;
    idle->queue_pinned = 0;
    idle->last_ipc_bump_tick = 0;
    idle->wait_next = NULL;
    idle->mbox_wait_next = NULL;
    idle->uthread = NULL;
    idle->user_cr3 = 0;
    init_thread_stack(idle);
    cpu_run_queues_init(cpu);
    cpu->current = idle;
}

void lwkt_init(void) {
    spin_init(&thread_pool_lock);
    for (int i = 0; i < MAX_THREADS; i++) {
        thread_pool[i].id = 0;
        thread_pool[i].state = THREAD_TERMINATED;
    }

    lwkt_cpu_init_idle();
    cpu_run_queues_init(this_cpu());
    thread_count = 0;
    sched_active = 0;
    ipi_targeted = 0;
    ipi_local = 0;
    ipi_broadcast = 0;

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

void lwkt_sched_ipi_cpu(struct cpu *dest) {
    if (!dest || !dest->online) {
        return;
    }
    struct cpu *self = this_cpu();
    if (dest == self) {
        ipi_local++;
        lwkt_preempt_request();
        return;
    }
    ipi_targeted++;
    lapic_ipi_send(dest->lapic_id, LAPIC_IPI_RESCHED_VECTOR);
}

void lwkt_sched_ipi_thread(struct lwkt_thread *t) {
    if (!t || !t->id) {
        return;
    }
    lwkt_sched_ipi_cpu(cpu_for_thread_wake(t));
}

/* Dedup by destination CPU — one resched IPI per core. */
void lwkt_sched_ipi_threads(struct lwkt_thread *const *threads, int count) {
    if (!threads || count <= 0) {
        return;
    }
    uint64_t seen = 0;
    for (int i = 0; i < count; i++) {
        struct lwkt_thread *t = threads[i];
        if (!t || !t->id) {
            continue;
        }
        struct cpu *dest = cpu_for_thread_wake(t);
        if (!dest || !dest->online) {
            continue;
        }
        if (dest->id < 64) {
            uint64_t bit = 1ULL << dest->id;
            if (seen & bit) {
                continue;
            }
            seen |= bit;
        }
        lwkt_sched_ipi_cpu(dest);
    }
}

void lwkt_sched_ipi_others(void) {
    if (cpu_online_count() > 1) {
        ipi_broadcast++;
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

void lwkt_ipc_bump(struct lwkt_thread *t) {
    ipc_bump_attempts++;
    if (!t) {
        return;
    }
    if (!ipc_bump_enabled) {
        ipc_bump_skipped_disabled++;
        /* Still IPI: receiver may be RUNNING in wait_edge (ping/PONG). */
        lwkt_sched_ipi_thread(t);
        return;
    }

    if (t->state != THREAD_READY) {
        /*
         * Not on a run queue (e.g. RUNNING in SYS_READ/msg wait_edge).
         * Queue bump does not apply; IPI so the CPU leaves hlt / reschedules.
         */
        lwkt_sched_ipi_thread(t);
        return;
    }

    uint32_t now = (uint32_t)timer_get_ticks();
    if (t->last_ipc_bump_tick != 0 &&
        (uint32_t)(now - t->last_ipc_bump_tick) < LWKT_IPC_BUMP_DEBOUNCE_TICKS) {
        ipc_bump_skipped_debounce++;
        return;
    }
    t->last_ipc_bump_tick = now;

    remove_from_queues(t);
    struct cpu *dest = pick_enqueue_cpu(NULL);
    if (!dest) {
        dest = this_cpu();
    }
    uint64_t irqf = cpu_irq_save();
    queue_lock(dest);
    enqueue_on_cpu_head(dest, t);
    queue_unlock(dest);
    cpu_irq_restore(irqf);
    ipc_bump_applied++;
    lwkt_sched_ipi_thread(t);
}

int lwkt_ipc_bump_mode(int mode) {
    if (mode == 0 || mode == 1) {
        ipc_bump_enabled = mode;
    }
    return ipc_bump_enabled;
}

static struct lwkt_thread *lwkt_create_locked(const char *name, void (*entry)(void *), void *arg,
                                              uint32_t priority, struct proc *user_p,
                                              struct uthread *bind_u) {
    struct lwkt_thread *t = NULL;
    for (int i = 0; i < MAX_THREADS; i++) {
        if (thread_pool[i].id == 0) {
            t = &thread_pool[i];
            break;
        }
    }
    if (!t) {
        return NULL;
    }

    if (!entry) {
        entry = worker_entry;
    }
    t->id = next_id++;

    if (!name || name[0] == '\0') {
        t->name[0] = 't';
        t->name[1] = '0' + (char)(t->id % 10);
        t->name[2] = '\0';
    } else {
        strcpy_local(t->name, name);
    }
    t->priority = priority;
    t->state = THREAD_READY;
    t->entry_point = entry;
    t->arg = arg;
    t->next = NULL;
    t->run_cpu = 0;
    t->last_cpu_executed = (uint32_t)-1;
    t->cpu_migrations = 0;
    t->pending_cr3_destroy = 0;
    t->yields = 0;
    t->quantum_expired = 0;
    t->quantum_forced_switches = 0;
    t->saved_kernel_rsp = 0;
    t->runner_resume_rsp = 0;
    t->runner_jmp.rsp = 0;
    t->in_syscall = 0;
    t->runner_reswitch = 0;
    t->pending_kill = 0;
    t->quantum_left = LWKT_QUANTUM_TICKS;
    t->quantum_force = 0;
    t->queue_pinned = 0;
    t->last_ipc_bump_tick = 0;
    t->wait_next = NULL;
    t->mbox_wait_next = NULL;
    t->mbox_slot = (uint8_t)((t - thread_pool) + 1);

    if (user_p) {
        t->user_proc = user_p;
        t->user_cr3 = user_p->cr3;
    } else {
        t->user_proc = NULL;
        t->user_cr3 = 0;
    }

    if (bind_u) {
        t->uthread = bind_u;
        bind_u->lwkt = t;
        /* Keep alloc_uthread_id() — do not alias TID to LWKT id (join races). */
        bind_u->state = UTHREAD_RUNNABLE;
    } else {
        t->uthread = NULL;
    }

    init_thread_stack(t);
    thread_count++;
    return t;
}

struct lwkt_thread *lwkt_create(const char *name, void (*entry)(void *), void *arg, uint32_t priority) {
    if (priority >= MAX_PRIORITY) {
        priority = MAX_PRIORITY - 1;
    }

    uint64_t irqf;
    pool_lock_irqsave(&irqf);
    if (thread_count >= MAX_THREADS) {
        pool_unlock_irqrestore(irqf);
        return NULL;
    }

    struct lwkt_thread *t = lwkt_create_locked(name, entry, arg, priority, NULL, NULL);
    pool_unlock_irqrestore(irqf);
    if (!t) {
        return NULL;
    }

    enqueue_thread(t);
    lwkt_sched_ipi_thread(t);
    return t;
}

struct lwkt_thread *lwkt_create_user(const char *name, void (*entry)(void *), void *arg,
                                     uint32_t priority, struct proc *user_p,
                                     struct uthread *bind_u) {
    if (priority >= MAX_PRIORITY) {
        priority = MAX_PRIORITY - 1;
    }

    uint64_t irqf;
    pool_lock_irqsave(&irqf);
    if (thread_count >= MAX_THREADS) {
        pool_unlock_irqrestore(irqf);
        return NULL;
    }

    struct lwkt_thread *t = lwkt_create_locked(name, entry, arg, priority, user_p, bind_u);
    pool_unlock_irqrestore(irqf);
    if (!t) {
        return NULL;
    }

    enqueue_thread(t);
    lwkt_sched_ipi_thread(t);
    return t;
}

int lwkt_destroy(uint32_t id) {
    if (id == 0) {
        return -1;
    }

    int wake_blocked = 0;
    int owner_cpu = -1;
    struct lwkt_thread *t;
    enum thread_state old;

    uint64_t irqf;
    pool_lock_irqsave(&irqf);
    t = find_thread(id);
    if (!t) {
        pool_unlock_irqrestore(irqf);
        return -1;
    }

    old = t->state;
    owner_cpu = thread_owner_cpu_id(t);
    int self_current = owner_cpu >= 0 &&
                       this_cpu() &&
                       owner_cpu == (int)this_cpu()->id;

    if (owner_cpu >= 0 && !self_current) {
        t->pending_kill = 1;
        t->entry_point = NULL;
        pool_unlock_irqrestore(irqf);
        lwkt_sched_ipi_cpu(cpu_by_id((uint32_t)owner_cpu));
        return 0;
    }

    if (self_current) {
        t->pending_kill = 1;
        t->entry_point = NULL;
        pool_unlock_irqrestore(irqf);
        lwkt_thread_exit();
        return 0;
    }

    t->entry_point = NULL;
    pool_unlock_irqrestore(irqf);

    remove_from_queues(t);

    if (old == THREAD_BLOCKED) {
        t->pending_kill = 1;
        t->state = THREAD_READY;
        enqueue_thread(t);
        wake_blocked = 1;
    } else {
        msgport_clear_slot(t->mbox_slot);
        msgport_unregister_id(t->id);
        pool_lock_irqsave(&irqf);
        t->state = THREAD_TERMINATED;
        t->rsp = 0;
        t->id = 0;
        thread_count--;
        pool_unlock_irqrestore(irqf);
    }

    if (wake_blocked) {
        msgport_wakeup(t);
        lwkt_sched_ipi_thread(t);
    }
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

#define LWKT_LIST_SNAP_MAX (MAX_THREADS + 8)

struct lwkt_list_snap {
    struct lwkt_thread *t;
};

static int lwkt_list_collect(struct lwkt_list_snap *snap, int max,
                             struct lwkt_thread *current) {
    int n = 0;
    uint64_t irqf = cpu_irq_save();

    for (uint32_t ci = 0; ci < cpu_online_count() && n < max; ci++) {
        struct cpu *c = cpu_by_id(ci);
        if (!c || !c->online) {
            continue;
        }
        queue_lock(c);
        for (int p = 0; p < MAX_PRIORITY && n < max; p++) {
            int guard = 0;
            for (struct lwkt_thread *t = c->run_queues[p];
                 t && n < max && guard < MAX_THREADS;
                 t = t->next, guard++) {
                if (t == current || t->state == THREAD_TERMINATED) {
                    continue;
                }
                snap[n++].t = t;
            }
        }
        queue_unlock(c);
    }

    spin_lock(&thread_pool_lock);
    for (int i = 0; i < MAX_THREADS && n < max; i++) {
        struct lwkt_thread *t = &thread_pool[i];
        if (t->id == 0 || t == current || t->state != THREAD_BLOCKED) {
            continue;
        }
        snap[n++].t = t;
    }
    spin_unlock(&thread_pool_lock);
    cpu_irq_restore(irqf);
    return n;
}

void lwkt_list(void) {
    struct cpu *cpu = this_cpu();
    struct lwkt_thread *current = cpu ? cpu->current : NULL;
    struct lwkt_list_snap snap[LWKT_LIST_SNAP_MAX];
    int snap_n;
    int total;
    uint64_t irqf;

    console_writestring("\nLWKT  Slot Proc  Name            Prio  State      CPU CR3            Qexp/Qsw/Mig\n");
    console_writestring("----  ---- ----  --------------  ----  ---------  --- --------------- ------------\n");

    snap_n = lwkt_list_collect(snap, LWKT_LIST_SNAP_MAX, current);

    pool_lock_irqsave(&irqf);
    total = thread_count;
    pool_unlock_irqrestore(irqf);

    if (current) {
        print_thread_row(current, 1);
    }

    for (int i = 0; i < snap_n; i++) {
        print_thread_row(snap[i].t, 0);
    }

    console_writestring("\nTotal: ");
    console_write_dec((uint64_t)total + cpu_online_count());
    console_writestring(" threads (incl. idle per CPU)\n");
    console_writestring("IPC bump mode=");
    console_write_dec((uint64_t)ipc_bump_enabled);
    console_writestring(" attempts=");
    console_write_dec(ipc_bump_attempts);
    console_writestring(" applied=");
    console_write_dec(ipc_bump_applied);
    console_writestring(" debounce_skip=");
    console_write_dec(ipc_bump_skipped_debounce);
    console_writestring(" disabled_skip=");
    console_write_dec(ipc_bump_skipped_disabled);
    console_putchar('\n');
    console_writestring("IPI targeted=");
    console_write_dec(ipi_targeted);
    console_writestring(" local=");
    console_write_dec(ipi_local);
    console_writestring(" broadcast=");
    console_write_dec(ipi_broadcast);
    console_putchar('\n');
}

void lwkt_smp_balance(void) {
    struct {
        uint32_t id;
        uint64_t switches;
        uint64_t steals;
        uint64_t pulls;
        unsigned runners_run;
        unsigned runners_ready;
        unsigned lwkt_ready;
        char tname[16];
    } snap[MAX_CPUS];
    uint32_t n = 0;

    console_writestring("\nSMP balance (KSE user uthreads per LWKT):\n");
    console_writestring("CPU  Switches  Steals  Pulls  Current          Runners(run/rdy)  LWKT-ready\n");
    console_writestring("---  ---------  ------  -----  ---------------  ----------------  ----------\n");

    uint64_t irqf = cpu_irq_save();
    for (uint32_t i = 0; i < cpu_online_count() && n < MAX_CPUS; i++) {
        struct cpu *c = cpu_by_id(i);
        if (!c || !c->online) {
            continue;
        }

        queue_lock(c);
        snap[n].id = c->id;
        snap[n].switches = c->switches;
        snap[n].steals = c->steals;
        snap[n].pulls = c->same_proc_pulls;
        snap[n].runners_run = 0;
        snap[n].runners_ready = 0;
        snap[n].lwkt_ready = 0;

        struct lwkt_thread *tcur = c->current;
        if (tcur && tcur != &c->idle && tcur->user_proc) {
            snap[n].runners_run = 1;
        }
        if (tcur) {
            strcpy_local(snap[n].tname, tcur->name);
        } else {
            snap[n].tname[0] = '-';
            snap[n].tname[1] = '\0';
        }

        for (uint32_t p = 0; p < MAX_PRIORITY; p++) {
            for (struct lwkt_thread *t = c->run_queues[p]; t; t = t->next) {
                if (t == &c->idle || t->state != THREAD_READY) {
                    continue;
                }
                snap[n].lwkt_ready++;
                if (t->user_proc) {
                    snap[n].runners_ready++;
                }
            }
        }
        queue_unlock(c);
        n++;
    }
    cpu_irq_restore(irqf);

    for (uint32_t i = 0; i < n; i++) {
        write_u64_padded((uint64_t)snap[i].id, 3);
        console_writestring("  ");
        write_u64_padded(snap[i].switches, 9);
        console_writestring("  ");
        write_u64_padded(snap[i].steals, 6);
        console_writestring("  ");
        write_u64_padded(snap[i].pulls, 5);
        console_writestring("  ");
        write_padded(snap[i].tname, 15);
        console_writestring("  ");
        write_ratio_padded((uint64_t)snap[i].runners_run, (uint64_t)snap[i].runners_ready, 16);
        console_writestring("  ");
        write_u64_padded((uint64_t)snap[i].lwkt_ready, 10);
        console_putchar('\n');
    }

    console_writestring("IPI targeted=");
    console_write_dec(ipi_targeted);
    console_writestring(" local=");
    console_write_dec(ipi_local);
    console_writestring(" broadcast=");
    console_write_dec(ipi_broadcast);
    console_putchar('\n');
    console_writestring(
        "Note: each user uthread is a schedulable LWKT (KSE); "
        "more CPUs help parallel uthreads across processes.\n");
}

struct lwkt_thread *lwkt_curthread(void) {
    struct cpu *cpu = this_cpu();
    return cpu ? cpu->current : NULL;
}

int lwkt_in_usersyscall(void) {
    struct lwkt_thread *cur = lwkt_curthread();
    return cur && cur->in_syscall;
}

void lwkt_syscall_wait_edge(void) {
    /*
     * int 0x80 uses syscall_stack — not safe to lwkt_switch() here.
     * Prefer returning when UART has data (no COM1 IRQ). Do not longjmp
     * out of blocking SYS_READ: that caused yield storms and lost serial
     * input under SMP after repeated exec.
     */
    if (serial_has_char()) {
        return;
    }
    __asm__ volatile("sti; hlt" ::: "memory");
}

int lwkt_syscall_resched(int64_t retry_ret) {
    if (!lwkt_in_usersyscall()) {
        return 0;
    }
    struct lwkt_thread *cur = lwkt_curthread();
    if (cur && cur->runner_reswitch) {
        return 1;
    }
    struct uthread *u = uthread_current();
    if (u) {
        u->user_syscall_ret = (uint64_t)retry_ret;
    }
    lwkt_syscall_wait_edge();
    return 1;
}

int lwkt_thread_count(void) {
    return thread_count;
}

static void lwkt_apply_cr3(struct lwkt_thread *t) {
    uint64_t cr3 = t && t->user_cr3 ? t->user_cr3 : vmm_kernel_cr3();
    vmm_switch(cr3);
}

static void lwkt_apply_tss(struct lwkt_thread *t) {
    if (!t) {
        return;
    }
    if (t->user_proc || (t->uthread && t->uthread->type == UTHREAD_USER)) {
        tss_set_rsp0(lwkt_thread_syscall_rsp0(t));
    }
}

void lwkt_bootstrap_first(void) {
    struct cpu *cpu = this_cpu();
    uint64_t irqf = cpu_irq_save();

    struct lwkt_thread *next = dequeue_thread(cpu, NULL);
    if (!next) {
        next = &cpu->idle;
    }

    cpu->idle.state = THREAD_READY;
    cpu->current = next;
    next->state = THREAD_RUNNING;
    cpu_irq_restore(irqf);

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

void lwkt_timer_tick(void) {
    struct cpu *cpu = this_cpu();
    if (!cpu || !cpu->sched_active) {
        return;
    }

    struct lwkt_thread *cur = cpu->current;
    if (!cur || cur == &cpu->idle || cur->state != THREAD_RUNNING) {
        return;
    }

    if (cur->quantum_left > 0) {
        cur->quantum_left--;
    }
    if (cur->quantum_left == 0) {
        cur->quantum_expired++;
        cur->quantum_left = LWKT_QUANTUM_TICKS;
        cur->quantum_force = 1;
        lwkt_preempt_request();
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

    if (cur != &cpu->idle && cur->pending_kill && !cur->in_syscall) {
        cpu->preempt_requested = 0;
        if (cur->uthread && cur->uthread->type == UTHREAD_USER) {
            cur->runner_reswitch = 1;
            runner_longjmp(&cur->runner_jmp, 1);
        }
        if (cur->user_proc) {
            cur->runner_reswitch = 1;
            runner_longjmp(&cur->runner_jmp, 1);
        }
        lwkt_thread_exit();
        return;
    }

    if (lwkt_in_usersyscall()) {
        return;
    }

    if (cur->user_proc && cur->user_proc->current_uthread) {
        uint64_t hw_rsp;
        uint64_t lo = (uint64_t)(uintptr_t)cur->stack;
        uint64_t hi = lo + STACK_SIZE;
        __asm__ volatile("mov %%rsp, %0" : "=r"(hw_rsp));
        if (hw_rsp < lo || hw_rsp >= hi) {
            cpu->preempt_requested = 0;
            return;
        }
    }

    int other = 0;
    uint64_t irqf = cpu_irq_save();
    if (spin_trylock(&cpu->queue_lock)) {
        for (uint32_t p = 0; p < MAX_PRIORITY; p++) {
            for (struct lwkt_thread *t = cpu->run_queues[p]; t; t = t->next) {
                if (t != cur && t->state == THREAD_READY) {
                    other = 1;
                    break;
                }
            }
            if (other) {
                break;
            }
        }
        spin_unlock(&cpu->queue_lock);
    } else {
        /* Contended local queue — assume work may exist. */
        other = 1;
    }
    if (!other) {
        for (uint32_t ci = 0; ci < cpu_online_count(); ci++) {
            struct cpu *c = cpu_by_id(ci);
            if (!c || !c->online || c == cpu) {
                continue;
            }
            if (!spin_trylock(&c->queue_lock)) {
                other = 1;
                break;
            }
            for (uint32_t p = 0; p < MAX_PRIORITY; p++) {
                for (struct lwkt_thread *t = c->run_queues[p]; t; t = t->next) {
                    if (t->state == THREAD_READY) {
                        other = 1;
                        break;
                    }
                }
                if (other) {
                    break;
                }
            }
            spin_unlock(&c->queue_lock);
            if (other) {
                break;
            }
        }
    }
    cpu_irq_restore(irqf);

    if (!other && cur == &cpu->idle) {
        cpu->preempt_requested = 0;
        return;
    }

    if (!other && !cur->quantum_force) {
        cpu->preempt_requested = 0;
        return;
    }

    if (cur->quantum_force) {
        cur->quantum_forced_switches++;
    }
    cpu->preempt_requested = 0;
    lwkt_switch();
}

void lwkt_switch(void) {
    struct cpu *cpu = this_cpu();
    struct lwkt_thread *prev = cpu->current;
    uint64_t irqf = cpu_irq_save();

    if (prev && prev->in_syscall) {
        cpu_irq_restore(irqf);
        return;
    }

    /*
     * Enqueue before dequeue (baseline). queue_pinned keeps the yielder
     * unstealable until this (or another local) dequeue selects it — otherwise
     * a remote CPU can run it while switch_context still uses its stack.
     */
    if (prev && prev->state == THREAD_RUNNING) {
        prev->state = THREAD_READY;
        prev->yields++;
        prev->queue_pinned = 1;
        queue_lock(cpu);
        enqueue_on_cpu(cpu, prev);
        queue_unlock(cpu);
    }

    struct lwkt_thread *next = dequeue_thread(cpu, prev);
    next->queue_pinned = 0;

    if (next == prev) {
        next->state = THREAD_RUNNING;
        cpu_irq_restore(irqf);
        return;
    }

    if (next->state == THREAD_TERMINATED) {
        next = &cpu->idle;
    }

    next->state = THREAD_RUNNING;
    cpu->current = next;

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

    if (next != &cpu->idle && next->id != 0) {
        if (next->last_cpu_executed != (uint32_t)-1 && next->last_cpu_executed != cpu->id) {
            next->cpu_migrations++;
        }
        next->last_cpu_executed = cpu->id;
    }

    if (prev && prev->pending_cr3_destroy) {
        uint64_t destroy_cr3 = prev->pending_cr3_destroy;
        prev->pending_cr3_destroy = 0;
        vmm_aspace_destroy(destroy_cr3);
    }

    /*
     * Never save interrupt/syscall stack pointer into prev->rsp.
     * int 0x80 runs on the LWKT kernel stack (TSS.RSP0), so hw_rsp can
     * be inside [stack, stack+SIZE) during a syscall — not a switch frame.
     */
    if (prev && (prev->uthread || prev->user_proc)) {
        uint64_t hw_rsp;
        uint64_t lo = (uint64_t)(uintptr_t)prev->stack;
        uint64_t hi = lo + STACK_SIZE;
        __asm__ volatile("mov %%rsp, %0" : "=r"(hw_rsp));
        if (prev->in_syscall || hw_rsp < lo || hw_rsp >= hi) {
            __asm__ volatile("mov %0, %%rsp" :: "r"(prev->rsp) : "memory");
        }
    }

    cpu->switches++;
    switch_context(&prev->rsp, next->rsp);
    cpu_irq_restore(irqf);
}

void lwkt_yield(void) {
    struct cpu *cpu = this_cpu();
    struct lwkt_thread *cur = cpu ? cpu->current : NULL;
    if (cur && cur != &cpu->idle && cur->pending_kill && !cur->in_syscall) {
        if (cur->user_proc) {
            cur->runner_reswitch = 1;
            runner_longjmp(&cur->runner_jmp, 1);
        }
        lwkt_thread_exit();
        return;
    }
    lwkt_switch();
}

void lwkt_block(void) {
    struct cpu *cpu = this_cpu();
    struct lwkt_thread *cur = cpu ? cpu->current : NULL;
    if (!cur || cur->state != THREAD_RUNNING) {
        return;
    }
    /*
     * int 0x80 uses the LWKT kernel stack; lwkt_switch() from syscall
     * corrupts the saved RSP. Poll with hlt and let the caller retry.
     */
    if (lwkt_in_usersyscall()) {
        lwkt_syscall_wait_edge();
        return;
    }
    cur->state = THREAD_BLOCKED;
    lwkt_switch();
}

void lwkt_unblock(struct lwkt_thread *t) {
    if (!t || t->state != THREAD_BLOCKED) {
        return;
    }
    t->state = THREAD_READY;
    struct cpu *dest = enqueue_thread(t);
    lwkt_sched_ipi_cpu(dest);
}

void lwkt_nudge(struct lwkt_thread *t) {
    if (!t || t->state == THREAD_TERMINATED) {
        return;
    }

    struct cpu *dest = NULL;
    if (t->state == THREAD_BLOCKED) {
        t->state = THREAD_READY;
        dest = enqueue_thread(t);
    } else if (t->state == THREAD_READY) {
        if (!remove_from_queues(t)) {
            /* Stolen/claimed between check and remove — IPI owner only. */
            dest = cpu_for_thread_wake(t);
        } else {
            dest = cpu_by_id(t->run_cpu);
            if (!dest || !dest->online) {
                dest = this_cpu();
            }
            if (dest) {
                uint64_t irqf = cpu_irq_save();
                queue_lock(dest);
                enqueue_on_cpu_head(dest, t);
                queue_unlock(dest);
                cpu_irq_restore(irqf);
            }
        }
    } else {
        dest = cpu_for_thread_wake(t);
    }
    lwkt_sched_ipi_cpu(dest);
}

void lwkt_thread_exit(void) {
    struct cpu *cpu = this_cpu();
    if (cpu && cpu->current) {
        remove_from_queues(cpu->current);
        msgport_clear_slot(cpu->current->mbox_slot);
        msgport_unregister_id(cpu->current->id);
        cpu->current->state = THREAD_TERMINATED;
        cpu->current->entry_point = NULL;
        cpu->current->pending_kill = 0;
        cpu->current->in_syscall = 0;
        uint64_t irqf;
        pool_lock_irqsave(&irqf);
        cpu->current->id = 0;
        thread_count--;
        pool_unlock_irqrestore(irqf);
    }
    lwkt_yield();
    for (;;) {
        __asm__ volatile("hlt");
    }
}
