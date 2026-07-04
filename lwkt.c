#include "lwkt.h"
#include "console.h"
#include "gdt.h"
#include "interrupt.h"
#include "msgport.h"
#include "uthread.h"
#include "vmm.h"

#include <stddef.h>
#include <stdint.h>

extern void switch_context(uint64_t *old_rsp, uint64_t new_rsp);
extern void thread_bootstrap(void);

static struct lwkt_thread thread_pool[MAX_THREADS];
static struct lwkt_thread *run_queues[MAX_PRIORITY];
static struct lwkt_thread *current_thread;
static struct lwkt_thread idle_thread;
static int thread_count;
static uint32_t next_id = 1;
static volatile int preempt_requested;
static int sched_active;

static void strcpy_local(char *dst, const char *src) {
    while (*src) {
        *dst++ = *src++;
    }
    *dst = '\0';
}

static void enqueue_thread(struct lwkt_thread *t) {
    if (!t || t->state == THREAD_TERMINATED) {
        return;
    }

    uint32_t p = t->priority;
    t->next = NULL;

    if (!run_queues[p]) {
        run_queues[p] = t;
        return;
    }

    struct lwkt_thread *tail = run_queues[p];
    while (tail->next) {
        tail = tail->next;
    }
    tail->next = t;
}

static int remove_from_queues(struct lwkt_thread *t) {
    for (int p = 0; p < MAX_PRIORITY; p++) {
        struct lwkt_thread **slot = &run_queues[p];
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

static struct lwkt_thread *dequeue_thread(struct lwkt_thread *skip) {
    struct lwkt_thread *fallback = NULL;

    for (uint32_t p = 0; p < MAX_PRIORITY; p++) {
        struct lwkt_thread **slot = &run_queues[p];
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

    if (fallback) {
        remove_from_queues(fallback);
        return fallback;
    }

    return &idle_thread;
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
    for (uint32_t p = 0; p < MAX_PRIORITY; p++) {
        if (run_queues[p]) {
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

void lwkt_init(void) {
    for (int i = 0; i < MAX_PRIORITY; i++) {
        run_queues[i] = NULL;
    }
    for (int i = 0; i < MAX_THREADS; i++) {
        thread_pool[i].id = 0;
        thread_pool[i].state = THREAD_TERMINATED;
    }

    strcpy_local(idle_thread.name, "idle");
    idle_thread.id = 0;
    idle_thread.priority = LWKT_PRIO_LOW;
    idle_thread.state = THREAD_RUNNING;
    idle_thread.entry_point = idle_worker;
    idle_thread.arg = NULL;
    idle_thread.next = NULL;
    idle_thread.yields = 0;
    idle_thread.saved_kernel_rsp = 0;
    idle_thread.mbox_slot = 0;
    init_thread_stack(&idle_thread);

    current_thread = &idle_thread;
    thread_count = 0;
    preempt_requested = 0;
    sched_active = 0;

    msgport_init();
}

void lwkt_sched_start(void) {
    if (!sched_active) {
        timer_init();
        sched_active = 1;
    }
}

void lwkt_sched_stop(void) {
    timer_set_enabled(0);
    sched_active = 0;
}

struct lwkt_thread *lwkt_create(const char *name, void (*entry)(void *), void *arg, uint32_t priority) {
    if (priority >= MAX_PRIORITY) {
        priority = MAX_PRIORITY - 1;
    }
    if (thread_count >= MAX_THREADS) {
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
    t->user_cr3 = 0;
    t->pending_cr3_destroy = 0;
    t->yields = 0;
    t->saved_kernel_rsp = 0;
    t->mbox_slot = (uint8_t)((t - thread_pool) + 1);
    init_thread_stack(t);
    enqueue_thread(t);
    thread_count++;

    if (!sched_active) {
        lwkt_sched_start();
    }

    return t;
}

int lwkt_destroy(uint32_t id) {
    if (id == 0) {
        return -1;
    }

    struct lwkt_thread *t = find_thread(id);
    if (!t) {
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

    if (t != current_thread) {
        t->rsp = 0;
        t->id = 0;
        thread_count--;
    }

    return 0;
}

struct lwkt_thread *lwkt_find(uint32_t id) {
    if (id == 0) {
        return &idle_thread;
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
    if (t == current_thread) {
        console_writestring(" (current)");
    }
    console_writestring("\n  Yields:   ");
    console_write_dec(t->yields);
    console_writestring("\n  RSP:      0x");
    console_write_hex(t->rsp);
    console_putchar('\n');
}

void lwkt_list(void) {
    console_writestring("\nID  Name            Prio  State      CR3\n");
    console_writestring("--  --------------  ----  ---------  ---------------\n");

    if (current_thread) {
        console_write_dec(current_thread->id);
        console_writestring("  ");
        console_writestring(current_thread->name);
        console_writestring("  ");
        console_write_dec(current_thread->priority);
        console_writestring("     ");
        console_writestring(state_name(current_thread->state));
        console_writestring(" *  ");
        if (current_thread->user_cr3) {
            console_write_hex(current_thread->user_cr3);
        } else {
            console_writestring("-");
        }
        console_putchar('\n');
    }

    for (int p = 0; p < MAX_PRIORITY; p++) {
        for (struct lwkt_thread *t = run_queues[p]; t; t = t->next) {
            if (t == current_thread || t->state == THREAD_TERMINATED) {
                continue;
            }
            console_write_dec(t->id);
            console_writestring("  ");
            console_writestring(t->name);
            console_writestring("  ");
            console_write_dec(t->priority);
            console_writestring("     ");
            console_writestring(state_name(t->state));
            console_writestring("    ");
            if (t->user_cr3) {
                console_write_hex(t->user_cr3);
            } else {
                console_writestring("-");
            }
            console_putchar('\n');
        }
    }

    for (int i = 0; i < MAX_THREADS; i++) {
        struct lwkt_thread *t = &thread_pool[i];
        if (t->id == 0 || t == current_thread || t->state != THREAD_BLOCKED) {
            continue;
        }
        console_write_dec(t->id);
        console_writestring("  ");
        console_writestring(t->name);
        console_writestring("  ");
        console_write_dec(t->priority);
        console_writestring("     ");
        console_writestring(state_name(t->state));
        console_writestring("    ");
        if (t->user_cr3) {
            console_write_hex(t->user_cr3);
        } else {
            console_writestring("-");
        }
        console_putchar('\n');
    }

    console_writestring("\nTotal: ");
    console_write_dec((uint64_t)thread_count + 1);
    console_writestring(" threads (incl. idle)\n");
}

struct lwkt_thread *lwkt_curthread(void) {
    return current_thread;
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
    struct lwkt_thread *next = dequeue_thread(NULL);
    if (!next) {
        next = &idle_thread;
    }

    idle_thread.state = THREAD_READY;
    current_thread = next;
    next->state = THREAD_RUNNING;

    static uint64_t boot_saved_rsp;
    lwkt_apply_cr3(next);
    lwkt_apply_tss(next);
    switch_context(&boot_saved_rsp, next->rsp);
    __builtin_unreachable();
}

void lwkt_preempt_request(void) {
    if (sched_active) {
        preempt_requested = 1;
    }
}

void lwkt_preempt_check(void) {
    if (preempt_requested) {
        preempt_requested = 0;
        lwkt_switch();
    }
}

void lwkt_switch(void) {
    struct lwkt_thread *prev = current_thread;

    if (prev && prev->state == THREAD_RUNNING) {
        prev->state = THREAD_READY;
        prev->yields++;
        enqueue_thread(prev);
    }

    struct lwkt_thread *next = dequeue_thread(prev);
    if (next == prev) {
        next->state = THREAD_RUNNING;
        return;
    }

    if (next->state == THREAD_TERMINATED) {
        next = &idle_thread;
    }

    next->state = THREAD_RUNNING;
    current_thread = next;

    if (next->rsp == 0) {
        if (next != &idle_thread) {
            next = &idle_thread;
            next->state = THREAD_RUNNING;
            current_thread = next;
        }
        if (next->rsp == 0) {
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

    switch_context(&prev->rsp, next->rsp);
}

void lwkt_yield(void) {
    lwkt_switch();
}

void lwkt_block(void) {
    struct lwkt_thread *cur = current_thread;
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
    t->state = THREAD_READY;
    enqueue_thread(t);
}

void lwkt_thread_exit(void) {
    if (current_thread) {
        remove_from_queues(current_thread);
        msgport_clear_slot(current_thread->mbox_slot);
        current_thread->state = THREAD_TERMINATED;
        current_thread->entry_point = NULL;
        current_thread->id = 0;
        thread_count--;
    }
    lwkt_yield();
    for (;;) {
        __asm__ volatile("hlt");
    }
}
