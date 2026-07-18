#include "uthread.h"

#include "console.h"
#include "msgport.h"
#include "myos_abi.h"
#include "proc.h"
#include "proc_mutex.h"
#include "spinlock.h"
#include "user.h"

#include <stddef.h>
#include <stdint.h>

#define MAX_UTHREADS MAX_THREADS
#define MAX_UTHREAD_IDS 256

static struct uthread uthread_pool[MAX_UTHREADS];
static int join_exit_code[MAX_UTHREAD_IDS];
static volatile int join_exit_valid[MAX_UTHREAD_IDS];
static spinlock_t uthread_join_lock;
static uint32_t next_global_uthread_id = 1;

int uthread_ptr_valid(const struct uthread *u) {
    if (!u) {
        return 0;
    }
    uintptr_t lo = (uintptr_t)uthread_pool;
    uintptr_t hi = lo + sizeof(uthread_pool);
    uintptr_t p = (uintptr_t)u;
    if (p < lo || p >= hi) {
        return 0;
    }
    return ((p - lo) % sizeof(struct uthread)) == 0;
}

static struct uthread *alloc_slot(void);
static struct uthread *find_by_id(uint32_t id);
static void uthread_reset_slot(struct uthread *u);

static int join_take_result(uint32_t uthread_id, int *exit_code_out) {
    if (uthread_id == 0 || uthread_id >= MAX_UTHREAD_IDS) {
        return 0;
    }
    if (__atomic_load_n(&join_exit_valid[uthread_id], __ATOMIC_ACQUIRE) == 0) {
        return 0;
    }
    if (exit_code_out) {
        *exit_code_out = join_exit_code[uthread_id];
    }
    __atomic_store_n(&join_exit_valid[uthread_id], 0, __ATOMIC_RELEASE);
    return 1;
}

static int join_take_and_reap(uint32_t uthread_id, int *exit_code_out) {
    if (!join_take_result(uthread_id, exit_code_out)) {
        return 0;
    }
    struct uthread *u = find_by_id(uthread_id);
    if (u) {
        struct proc *joiner = proc_current();
        if (u->proc && joiner && u->proc != joiner) {
            /* Exit code is valid, but do not tear down another process's uthread. */
            return 1;
        }
        if (u->proc && proc_is_shell(u->proc)) {
            return 1;
        }
        if (u->proc) {
            proc_detach_uthread(u->proc, u);
        }
        uthread_reset_slot(u);
    }
    return 1;
}

static struct uthread *alloc_slot(void) {
    static int pool_inited;
    if (!pool_inited) {
        spin_init(&uthread_join_lock);
        pool_inited = 1;
    }
    for (int i = 0; i < MAX_UTHREADS; i++) {
        if (uthread_pool[i].uthread_id == 0 && uthread_pool[i].state != UTHREAD_ZOMBIE) {
            uthread_pool[i].slot = (uint32_t)i;
            return &uthread_pool[i];
        }
    }
    return NULL;
}

static struct uthread *find_by_id(uint32_t id) {
    if (id == 0) {
        return NULL;
    }
    for (int i = 0; i < MAX_UTHREADS; i++) {
        if (uthread_pool[i].uthread_id == id) {
            return &uthread_pool[i];
        }
    }
    return NULL;
}

static void uthread_reset_slot(struct uthread *u) {
    if (!u) {
        return;
    }
    u->uthread_id = 0;
    u->slot = 0;
    u->state = UTHREAD_RUNNABLE;
    u->priority = LWKT_PRIO_NORMAL;
    u->entry = NULL;
    u->arg = NULL;
    u->user_rip = 0;
    u->user_rsp = 0;
    u->user_rdi = 0;
    u->user_syscall_ret = 0;
    u->user_rbx = 0;
    u->user_rbp = 0;
    u->user_r12 = 0;
    u->user_r13 = 0;
    u->user_r14 = 0;
    u->user_r15 = 0;
    u->user_regs_valid = 0;
    u->user_arg = 0;
    u->user_stack_base = 0;
    u->exit_code = 0;
    u->join_waiter = NULL;
    u->proc = NULL;
    u->lwkt = NULL;
    u->next_in_proc = NULL;
    u->run_next = NULL;
    u->type = UTHREAD_KERNEL;
}

static void uthread_release_resources(struct uthread *u) {
    if (!u) {
        return;
    }
    if (u->type == UTHREAD_KERNEL && u->entry == msg_ping_worker && u->arg) {
        msg_ping_ctx_free((struct ping_ctx *)u->arg);
    }
    if (u->type == UTHREAD_USER && u->user_stack_base && u->proc && u->proc->cr3) {
        user_stack_free(u->proc, u->user_stack_base);
        u->user_stack_base = 0;
    }
    if (u->type == UTHREAD_USER && u->proc) {
        proc_detach_uthread(u->proc, u);
    }
    if (u->lwkt) {
        u->lwkt->uthread = NULL;
        u->lwkt->user_cr3 = 0;
    }
}

static void uthread_make_zombie(struct uthread *u) {
    uthread_release_resources(u);
    u->state = UTHREAD_ZOMBIE;
    u->lwkt = NULL;
}

static void uthread_record_join(struct uthread *u) {
    uint32_t id = u ? u->uthread_id : 0;
    if (id == 0 || id >= MAX_UTHREAD_IDS) {
        return;
    }
    if (u) {
        join_exit_code[id] = u->exit_code;
    }
    __atomic_store_n(&join_exit_valid[id], 1, __ATOMIC_RELEASE);
    /* Joiner sees the flag via ACQUIRE load; wake is uthread_wakeup_joiner(). */
}

static int uthread_join_zombie(struct uthread *u, struct proc *p, int *exit_code_out) {
    if (!u || u->state != UTHREAD_ZOMBIE || !p) {
        return 0;
    }
    if (u->proc != NULL && u->proc != p) {
        return 0;
    }
    if (exit_code_out) {
        *exit_code_out = u->exit_code;
    }
    if (u->uthread_id < MAX_UTHREAD_IDS) {
        __atomic_store_n(&join_exit_valid[u->uthread_id], 0, __ATOMIC_RELEASE);
    }
    if (u->proc) {
        proc_detach_uthread(u->proc, u);
    }
    uthread_reset_slot(u);
    return 1;
}

static void proc_sched_enqueue(struct proc *p, struct uthread *u) {
    if (!p || !u || u->state == UTHREAD_ZOMBIE) {
        return;
    }
    for (struct uthread *w = p->run_queue; w; w = w->run_next) {
        if (w == u) {
            return;
        }
    }
    u->run_next = p->run_queue;
    p->run_queue = u;
    if (u->state != UTHREAD_ZOMBIE && u->state != UTHREAD_BLOCKED) {
        u->state = UTHREAD_RUNNABLE;
    }
}

static void proc_sched_remove(struct proc *p, struct uthread *u) {
    if (!p || !u) {
        return;
    }
    struct uthread **slot = &p->run_queue;
    while (*slot) {
        if (*slot == u) {
            *slot = u->run_next;
            u->run_next = NULL;
            return;
        }
        slot = &(*slot)->run_next;
    }
}

static void uthread_wakeup_joiner(struct uthread *u) {
    if (!u || !u->join_waiter) {
        return;
    }
    struct uthread *w = u->join_waiter;
    u->join_waiter = NULL;
    if (!w->proc) {
        return;
    }
    w->state = UTHREAD_RUNNABLE;
    w->user_syscall_ret = (uint64_t)MYOS_ERR_AGAIN;
    if (w->proc->sched_mode == PROC_SCHED_KSE) {
        if (w->lwkt) {
            lwkt_unblock(w->lwkt);
            lwkt_nudge(w->lwkt);
        } else {
            proc_sched_nudge(w->proc);
        }
        return;
    }
    proc_sched_enqueue(w->proc, w);
    proc_sched_nudge(w->proc);
}

static int uthread_runner_schedulable(struct proc *p, struct uthread *u) {
    if (!p || !u || u->state != UTHREAD_RUNNABLE) {
        return 0;
    }
    if (u->lwkt && p->runner && u->lwkt != p->runner) {
        return 0;
    }
    return 1;
}

static int proc_sched_has_runnable_other(struct proc *p, struct uthread *skip) {
    if (!p) {
        return 0;
    }
    for (struct uthread *u = p->run_queue; u; u = u->run_next) {
        if (u != skip && uthread_runner_schedulable(p, u)) {
            return 1;
        }
    }
    return 0;
}

static struct uthread *proc_sched_pick_other(struct proc *p, struct uthread *skip) {
    if (!p || !p->run_queue) {
        return NULL;
    }

    struct uthread **bestp = NULL;
    struct uthread *best = NULL;
    uint32_t best_prio = MAX_PRIORITY;

    for (struct uthread **slot = &p->run_queue; *slot;) {
        struct uthread *u = *slot;
        if (u == skip || !uthread_runner_schedulable(p, u)) {
            slot = &u->run_next;
            continue;
        }
        if (!best || u->priority < best_prio) {
            best = u;
            best_prio = u->priority;
            bestp = slot;
        }
        slot = &u->run_next;
    }

    if (!best || !bestp) {
        return NULL;
    }

    *bestp = best->run_next;
    best->run_next = NULL;
    return best;
}

static struct uthread *proc_sched_pick(struct proc *p) {
    struct uthread *u = proc_sched_pick_other(p, NULL);
    if (u) {
        return u;
    }

    /* Heal RUNNABLE uthreads that lost their run_queue link. */
    for (struct uthread *walk = p->threads; walk; walk = walk->next_in_proc) {
        if (!uthread_runner_schedulable(p, walk)) {
            continue;
        }
        if (walk->uthread_id == 0 || walk->proc != p) {
            continue;
        }
        proc_sched_remove(p, walk);
        walk->run_next = NULL;
        return walk;
    }

    return NULL;
}

static int uthread_user_ctx_valid(const struct uthread *u) {
    if (!u) {
        return 0;
    }
    if (u->user_rip < MYOS_USER_BASE || u->user_rip >= MYOS_USER_STACK_TOP) {
        return 0;
    }
    if (u->user_rsp < MYOS_USER_STACK_BASE || u->user_rsp >= MYOS_USER_STACK_TOP) {
        return 0;
    }
    return 1;
}

void proc_runner_resched(struct proc *p) {
    if (!p) {
        return;
    }
    if (p->sched_mode != PROC_SCHED_RUNNER || !p->runner) {
        return;
    }
    if (p->runner->state == THREAD_BLOCKED) {
        lwkt_unblock(p->runner);
    } else {
        lwkt_sched_ipi_thread(p->runner);
    }
}

void proc_sched_nudge(struct proc *p) {
    if (!p) {
        return;
    }
    if (p->sched_mode == PROC_SCHED_RUNNER && p->runner) {
        proc_runner_resched(p);
        return;
    }

    struct lwkt_thread *ts[MAX_THREADS];
    int n = 0;
    for (struct uthread *u = p->threads; u && n < MAX_THREADS; u = u->next_in_proc) {
        if (u->lwkt && u->lwkt->id) {
            ts[n++] = u->lwkt;
        }
    }
    lwkt_sched_ipi_threads(ts, n);
}

static void uthread_return_to_runner(struct proc *p) {
    struct lwkt_thread *cur = lwkt_curthread();
    if (!cur || !p) {
        lwkt_thread_exit();
    }
    if (p->sched_mode == PROC_SCHED_KSE) {
        cur->runner_reswitch = 1;
        return;
    }
    if (p->runner != cur) {
        lwkt_thread_exit();
    }

    cur->runner_reswitch = 1;
}

static void uthread_exit_runner(struct proc *p, struct uthread *u) {
    struct lwkt_thread *cur = lwkt_curthread();
    if (cur) {
        cur->in_syscall = 0;
        cur->user_proc = NULL;
    }
    if (u) {
        uint64_t jirqf;
        spin_lock_irqsave(&uthread_join_lock, &jirqf);
        if (u->uthread_id < MAX_UTHREAD_IDS) {
            __atomic_store_n(&join_exit_valid[u->uthread_id], 0, __ATOMIC_RELEASE);
        }
        uthread_reset_slot(u);
        spin_unlock_irqrestore(&uthread_join_lock, jirqf);
    }
    uthread_reap_proc(p);
    proc_on_uthread_exit(p, NULL);
    lwkt_thread_exit();
}

static void proc_runner_entry(void *arg) {
    (void)arg;

    for (;;) {
        struct lwkt_thread *runner = lwkt_curthread();
        struct proc *p = runner ? runner->user_proc : NULL;
        int resumed;

        if (!p || p->pid == 0 || p->state != PROC_RUNNING) {
            lwkt_thread_exit();
        }
        /* Heal race: create may schedule before proc_start_runner assigns runner. */
        if (!p->runner) {
            p->runner = runner;
        }
        if (p->runner != runner) {
            lwkt_thread_exit();
        }

        resumed = runner_setjmp(&runner->runner_jmp);
        /*
         * setjmp/longjmp clobbers stack locals; reload from the LWKT thread
         * object after every return (first entry and longjmp resume).
         */
        runner = lwkt_curthread();
        p = runner ? runner->user_proc : NULL;
        if (!p || p->pid == 0 || p->state != PROC_RUNNING) {
            lwkt_thread_exit();
        }
        if (!p->runner) {
            p->runner = runner;
        }
        if (p->runner != runner) {
            lwkt_thread_exit();
        }

        if (runner->pending_kill) {
            runner->pending_kill = 0;
            runner->in_syscall = 0;
            p->current_uthread = NULL;
            proc_destroy(p);
            lwkt_thread_exit();
        }

        if (resumed != 0) {
            runner->in_syscall = 0;
            p->current_uthread = NULL;

            if (runner->runner_reswitch) {
                runner->runner_reswitch = 0;
            }
        }

        struct uthread *u = proc_sched_pick(p);
        if (!u) {
            lwkt_block();
            continue;
        }

        p->current_uthread = u;
        u->state = UTHREAD_RUNNING;
        runner->user_cr3 = p->cr3;

        if (!uthread_user_ctx_valid(u)) {
            console_writestring("\nproc-runner: invalid user ctx pid=");
            console_write_dec(p->pid);
            console_writestring(" tid=");
            console_write_dec(u->uthread_id);
            console_writestring(" rip=");
            console_write_hex(u->user_rip);
            console_writestring(" rsp=");
            console_write_hex(u->user_rsp);
            console_putchar('\n');
            proc_sched_remove(p, u);
            p->current_uthread = NULL;
            proc_destroy(p);
            lwkt_thread_exit();
        }

        user_enter(u->user_rip, u->user_rsp, u->user_rdi, u->user_syscall_ret,
                   &runner->saved_kernel_rsp, u);

        runner->in_syscall = 0;
        p->current_uthread = NULL;
    }
}

static void user_kse_entry(void *arg) {
    struct uthread *u = (struct uthread *)arg;
    for (;;) {
        struct lwkt_thread *cur = lwkt_curthread();
        struct proc *p = (u && u->proc) ? u->proc : NULL;
        int resumed;

        if (!cur || !u || !p || p->pid == 0 || p->state != PROC_RUNNING || u->lwkt != cur) {
            lwkt_thread_exit();
        }

        resumed = runner_setjmp(&cur->runner_jmp);
        cur = lwkt_curthread();
        p = (u && u->proc) ? u->proc : NULL;
        if (!cur || !u || !p || p->pid == 0 || p->state != PROC_RUNNING || u->lwkt != cur) {
            lwkt_thread_exit();
        }

        if (cur->pending_kill) {
            cur->pending_kill = 0;
            cur->in_syscall = 0;
            if (u && u->proc) {
                uint64_t jirqf;
                spin_lock_irqsave(&uthread_join_lock, &jirqf);
                u->exit_code = -1;
                uthread_record_join(u);
                uthread_make_zombie(u);
                spin_unlock_irqrestore(&uthread_join_lock, jirqf);
                uthread_wakeup_joiner(u);
            }
            proc_destroy(p);
            lwkt_thread_exit();
        }

        if (resumed != 0) {
            cur->in_syscall = 0;
            if (cur->runner_reswitch) {
                cur->runner_reswitch = 0;
                lwkt_yield();
            }
            continue;
        }

        if (!uthread_user_ctx_valid(u)) {
            if (u && u->proc) {
                uint64_t jirqf;
                spin_lock_irqsave(&uthread_join_lock, &jirqf);
                u->exit_code = -1;
                uthread_record_join(u);
                uthread_make_zombie(u);
                spin_unlock_irqrestore(&uthread_join_lock, jirqf);
                uthread_wakeup_joiner(u);
            }
            proc_destroy(p);
            lwkt_thread_exit();
        }

        u->state = UTHREAD_RUNNING;
        cur->user_proc = p;
        cur->user_cr3 = p->cr3;
        user_enter(u->user_rip, u->user_rsp, u->user_rdi, u->user_syscall_ret,
                   &cur->saved_kernel_rsp, u);
        cur->in_syscall = 0;
        if (u->state == UTHREAD_RUNNING) {
            u->state = UTHREAD_RUNNABLE;
        }
    }
}

void uthread_reap_proc(struct proc *p) {
    if (!p) {
        return;
    }

    for (;;) {
        struct uthread *t;
        uint64_t irqf;

        spin_lock_irqsave(&uthread_join_lock, &irqf);
        t = p->threads;
        spin_unlock_irqrestore(&uthread_join_lock, irqf);
        if (!t) {
            break;
        }

        proc_detach_uthread(p, t);

        spin_lock_irqsave(&uthread_join_lock, &irqf);
        if (t->uthread_id < MAX_UTHREAD_IDS) {
            __atomic_store_n(&join_exit_valid[t->uthread_id], 0, __ATOMIC_RELEASE);
        }
        uthread_reset_slot(t);
        spin_unlock_irqrestore(&uthread_join_lock, irqf);
    }
}

void uthread_discard_zombie(struct uthread *u) {
    if (!u || u->state != UTHREAD_ZOMBIE) {
        return;
    }
    uint64_t irqf;
    spin_lock_irqsave(&uthread_join_lock, &irqf);
    uthread_reset_slot(u);
    spin_unlock_irqrestore(&uthread_join_lock, irqf);
}

int proc_start_runner(struct proc *p, uint32_t lwkt_priority) {
    if (!p || p->runner) {
        return p && p->runner ? 0 : -1;
    }

    char name[16];
    name[0] = 'p';
    int i = 1;
    uint32_t pid = p->pid;
    if (pid >= 10) {
        name[i++] = '0' + (char)((pid / 10) % 10);
    }
    name[i++] = '0' + (char)(pid % 10);
    name[i] = '\0';

    struct lwkt_thread *t = lwkt_create_user(name, proc_runner_entry, p, lwkt_priority, p, NULL);
    if (!t) {
        return -2;
    }

    p->runner = t;
    return 0;
}

static uint32_t alloc_uthread_id(void) {
    for (uint32_t n = 0; n < MAX_UTHREAD_IDS; n++) {
        uint32_t id = next_global_uthread_id++;
        if (id == 0) {
            id = next_global_uthread_id++;
        }
        if (!find_by_id(id)) {
            return id;
        }
    }
    return 0;
}

static struct uthread *uthread_alloc_user(struct proc *p, uint64_t rip, uint64_t rsp,
                                          uint64_t arg, uint64_t stack_base, uint32_t priority,
                                          int runner_queue) {
    if (!p || p->state != PROC_RUNNING || !p->cr3) {
        return NULL;
    }

    struct uthread *u = alloc_slot();
    if (!u) {
        return NULL;
    }

    uint32_t id = alloc_uthread_id();
    if (id == 0) {
        uthread_reset_slot(u);
        return NULL;
    }

    u->uthread_id = id;
    u->type = UTHREAD_USER;
    u->state = UTHREAD_RUNNABLE;
    u->priority = priority;
    u->proc = p;
    u->user_rip = rip;
    u->user_rsp = rsp;
    u->user_rdi = arg;
    u->user_arg = arg;
    u->user_stack_base = stack_base;
    u->user_syscall_ret = 0;
    u->user_rbx = 0;
    u->user_rbp = 0;
    u->user_r12 = 0;
    u->user_r13 = 0;
    u->user_r14 = 0;
    u->user_r15 = 0;
    u->user_regs_valid = 0;

    proc_attach_uthread(p, u);
    if (runner_queue) {
        proc_sched_enqueue(p, u);
        proc_runner_resched(p);
    }
    return u;
}

static void uthread_abort_create(struct proc *p, struct uthread *u) {
    uint64_t irqf;
    spin_lock_irqsave(&uthread_join_lock, &irqf);
    proc_sched_remove(p, u);
    uthread_release_resources(u);
    if (u->uthread_id < MAX_UTHREAD_IDS) {
        __atomic_store_n(&join_exit_valid[u->uthread_id], 0, __ATOMIC_RELEASE);
    }
    uthread_reset_slot(u);
    spin_unlock_irqrestore(&uthread_join_lock, irqf);
}

static int uthread_attach_kse_lwkt(struct proc *p, struct uthread *u, uint32_t priority) {
    char name[16];
    name[0] = 'u';
    int i = 1;
    uint32_t pid = p->pid;
    if (pid >= 10) {
        name[i++] = '0' + (char)((pid / 10) % 10);
    }
    name[i++] = '0' + (char)(pid % 10);
    name[i] = '\0';

    struct lwkt_thread *t = lwkt_create_user(name, user_kse_entry, u, priority, p, u);
    return t ? 0 : -1;
}

static int uthread_finish_user_create(struct proc *p, struct uthread *u, uint32_t priority) {
    if (!p || !u) {
        return -1;
    }
    if (p->sched_mode == PROC_SCHED_KSE) {
        if (uthread_attach_kse_lwkt(p, u, priority) != 0) {
            uthread_abort_create(p, u);
            return -4;
        }
    }
    return 0;
}

static void uthread_bind_lwkt(struct uthread *u, struct lwkt_thread *t) {
    u->uthread_id = t->id;
    u->lwkt = t;
    t->uthread = u;
    u->state = UTHREAD_RUNNABLE;
}

static void uthread_trampoline(void *arg) {
    struct uthread *u = (struct uthread *)arg;
    if (!u || !u->entry) {
        uthread_exit();
        return;
    }
    u->entry(u->arg);
    uthread_exit();
}

struct uthread *uthread_spawn(const char *name, void (*entry)(void *), void *arg, uint32_t priority) {
    struct uthread *u = alloc_slot();
    if (!u) {
        return NULL;
    }

    u->entry = entry ? entry : lwkt_default_worker;
    u->arg = arg;
    u->type = UTHREAD_KERNEL;
    u->state = UTHREAD_RUNNABLE;
    u->priority = priority;
    u->proc = NULL;

    struct lwkt_thread *t = lwkt_create(name, uthread_trampoline, u, priority);
    if (!t) {
        uthread_reset_slot(u);
        return NULL;
    }

    uthread_bind_lwkt(u, t);
    return u;
}

struct uthread *uthread_spawn_in_proc(struct proc *p, uint64_t rip, uint64_t rsp,
                                      uint64_t arg, uint64_t stack_base, uint32_t priority) {
    int runner_queue = (p && p->sched_mode == PROC_SCHED_RUNNER) ? 1 : 0;
    struct uthread *u = uthread_alloc_user(p, rip, rsp, arg, stack_base, priority, runner_queue);
    if (!u) {
        return NULL;
    }
    if (uthread_finish_user_create(p, u, priority) != 0) {
        return NULL;
    }
    return u;
}

int uthread_create_in_proc(struct proc *p, uint64_t rip, uint64_t arg, uint32_t priority) {
    if (!p || p->state != PROC_RUNNING || !p->cr3) {
        return -1;
    }

    uint64_t rsp, base;
    if (user_stack_alloc(p, &rsp, &base) != 0) {
        return -2;
    }

    int runner_queue = (p->sched_mode == PROC_SCHED_RUNNER) ? 1 : 0;
    struct uthread *u = uthread_alloc_user(p, rip, rsp, arg, base, priority, runner_queue);
    if (!u) {
        user_stack_free(p, base);
        return -3;
    }

    if (uthread_finish_user_create(p, u, priority) != 0) {
        user_stack_free(p, base);
        return -4;
    }

    return (int)u->uthread_id;
}

int uthread_create_in_proc_ex(struct proc *p, uint64_t rip, uint64_t arg, uint32_t priority,
                              uint32_t flags) {
    (void)flags;
    return uthread_create_in_proc(p, rip, arg, priority);
}

int uthread_kill(uint32_t uthread_id) {
    struct uthread *u = find_by_id(uthread_id);
    if (!u) {
        return -1;
    }

    if (u->proc && proc_is_shell(u->proc)) {
        return -2;
    }

    if (u->type == UTHREAD_KERNEL && u->lwkt) {
        int rc = lwkt_destroy(u->lwkt->id);
        if (rc == 0) {
            uthread_reset_slot(u);
        }
        return rc;
    }

    if (!u->proc) {
        return -1;
    }

    u->exit_code = -1;
    struct proc *up = u->proc;
    uint64_t jirqf;
    spin_lock_irqsave(&uthread_join_lock, &jirqf);
    uthread_record_join(u);
    uthread_make_zombie(u);
    spin_unlock_irqrestore(&uthread_join_lock, jirqf);
    uthread_wakeup_joiner(u);

    if (up && (up->current_uthread == u ||
               (u->lwkt && lwkt_curthread() == u->lwkt))) {
        uthread_return_to_runner(up);
    }
    uthread_reset_slot(u);
    return 0;
}

struct uthread *uthread_lookup(uint32_t uthread_id) {
    return find_by_id(uthread_id);
}

struct uthread *uthread_current(void) {
    struct lwkt_thread *t = lwkt_curthread();
    if (t && t->uthread && t->uthread->type == UTHREAD_USER) {
        return t->uthread;
    }
    struct proc *p = proc_current();
    return p ? p->current_uthread : NULL;
}

void uthread_yield(void) {
    struct proc *p = proc_current();
    struct lwkt_thread *self = lwkt_curthread();
    struct uthread *su = uthread_current();

    if (p && su && self && su->lwkt == self) {
        if (self->pending_kill) {
            self->runner_reswitch = 1;
            uthread_return_to_runner(p);
            return;
        }
        if (lwkt_in_usersyscall()) {
            self->runner_reswitch = 1;
            return;
        }
        lwkt_yield();
        return;
    }

    struct lwkt_thread *runner = p ? p->runner : NULL;
    struct uthread *cur = p ? p->current_uthread : NULL;

    if (!p || !runner || !cur) {
        lwkt_preempt_request();
        return;
    }

    if (!lwkt_in_usersyscall()) {
        lwkt_preempt_request();
        return;
    }

    if (runner->pending_kill) {
        uthread_return_to_runner(p);
        return;
    }

    if (cur->state != UTHREAD_BLOCKED) {
        proc_sched_enqueue(p, cur);
    }

    if (!proc_sched_has_runnable_other(p, cur)) {
        if (runner->pending_kill) {
            uthread_return_to_runner(p);
            return;
        }
        if (lwkt_in_usersyscall()) {
            lwkt_syscall_wait_edge();
        }
        return;
    }

    cur->user_syscall_ret = (uint64_t)MYOS_ERR_AGAIN;
    uthread_return_to_runner(p);
}

void uthread_exit(void) {
    struct proc *p = proc_current();
    struct uthread *u = uthread_current();
    struct lwkt_thread *self_lwkt = lwkt_curthread();

    if (!u) {
        struct lwkt_thread *cur = lwkt_curthread();
        if (cur && cur->uthread) {
            u = cur->uthread;
            p = u->proc;
        }
    }

    if (u && u->proc) {
        proc_mutex_abandon_all(u->proc->mutexes, PROC_MUTEX_MAX, u);
    }

    int kse_user_exit = u && self_lwkt && u->lwkt == self_lwkt && u->type == UTHREAD_USER;

    if (u) {
        uint64_t jirqf;
        spin_lock_irqsave(&uthread_join_lock, &jirqf);
        uthread_record_join(u);
        uthread_make_zombie(u);
        spin_unlock_irqrestore(&uthread_join_lock, jirqf);
        uthread_wakeup_joiner(u);
    }

    if (kse_user_exit || !p || !p->runner) {
        if (p && p->uthread_count == 0) {
            proc_on_uthread_exit(p, u);
        }
        lwkt_thread_exit();
    }

    if (p->uthread_count == 0) {
        uthread_exit_runner(p, u);
        return;
    }

    int live = 0;
    for (struct uthread *walk = p->threads; walk; walk = walk->next_in_proc) {
        if (walk->state != UTHREAD_ZOMBIE && walk->uthread_id != 0) {
            live++;
        }
    }

    if (live == 0) {
        uthread_reap_proc(p);
        uthread_exit_runner(p, u);
        return;
    }

    uthread_return_to_runner(p);
}

int uthread_join(uint32_t uthread_id, int *exit_code_out) {
    struct proc *p = proc_current();
    if (!p || uthread_id == 0) {
        return -1;
    }

    for (;;) {
        uint64_t irqf;
        struct uthread *u;
        struct uthread *self = uthread_current();

        spin_lock_irqsave(&uthread_join_lock, &irqf);
        if (join_take_and_reap(uthread_id, exit_code_out)) {
            spin_unlock_irqrestore(&uthread_join_lock, irqf);
            return 0;
        }

        u = find_by_id(uthread_id);
        if (!u) {
            if (join_take_and_reap(uthread_id, exit_code_out)) {
                spin_unlock_irqrestore(&uthread_join_lock, irqf);
                return 0;
            }
            spin_unlock_irqrestore(&uthread_join_lock, irqf);
            return MYOS_ERR_NOENT;
        }

        if (u->proc != NULL && u->proc != p) {
            if (join_take_and_reap(uthread_id, exit_code_out)) {
                spin_unlock_irqrestore(&uthread_join_lock, irqf);
                return 0;
            }
            spin_unlock_irqrestore(&uthread_join_lock, irqf);
            return MYOS_ERR_NOENT;
        }

        if (u->state == UTHREAD_ZOMBIE) {
            if (uthread_join_zombie(u, p, exit_code_out)) {
                spin_unlock_irqrestore(&uthread_join_lock, irqf);
                return 0;
            }
            if (join_take_and_reap(uthread_id, exit_code_out)) {
                spin_unlock_irqrestore(&uthread_join_lock, irqf);
                return 0;
            }
            spin_unlock_irqrestore(&uthread_join_lock, irqf);
            return MYOS_ERR_NOENT;
        }

        if (self) {
            u->join_waiter = self;
            self->user_syscall_ret = (uint64_t)MYOS_ERR_AGAIN;
            if (p->sched_mode == PROC_SCHED_RUNNER) {
                proc_sched_remove(p, self);
                self->state = UTHREAD_BLOCKED;
            }
        }
        /* Close lost-wakeup: exit may land between the first take and waiter install. */
        if (join_take_and_reap(uthread_id, exit_code_out)) {
            if (self && u) {
                u->join_waiter = NULL;
            }
            spin_unlock_irqrestore(&uthread_join_lock, irqf);
            return 0;
        }
        spin_unlock_irqrestore(&uthread_join_lock, irqf);

        proc_sched_nudge(p);
        uthread_yield();
        return MYOS_ERR_AGAIN;
    }
}

uint32_t uthread_slot_of(const struct uthread *u) {
    if (!u) {
        return (uint32_t)-1;
    }
    return u->slot;
}

int uthread_index_in_proc(const struct uthread *u) {
    if (!u || !u->proc || !u->proc->threads) {
        return -1;
    }
    int idx = 1;
    for (struct uthread *walk = u->proc->threads; walk; walk = walk->next_in_proc) {
        if (walk == u) {
            return idx;
        }
        idx++;
    }
    return -1;
}

static const char *uthread_type_name(enum uthread_type type) {
    return type == UTHREAD_USER ? "user" : "kernel";
}

static const char *uthread_state_name(enum uthread_state st) {
    switch (st) {
        case UTHREAD_RUNNABLE: return "ready";
        case UTHREAD_RUNNING: return "running";
        case UTHREAD_BLOCKED: return "blocked";
        case UTHREAD_ZOMBIE: return "zombie";
        default: return "?";
    }
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

void uthread_list(void) {
    console_writestring("\nSlot  PID  #InProc  TID   Type    State       Prio  Name\n");
    console_writestring("----  ---  -------  ----  ------  ----------  ----  --------------\n");

    int count = 0;
    for (int i = 0; i < MAX_UTHREADS; i++) {
        struct uthread *u = &uthread_pool[i];
        if (u->uthread_id == 0 && u->state != UTHREAD_ZOMBIE) {
            continue;
        }
        count++;

        write_u64_padded((uint64_t)uthread_slot_of(u), 4);
        console_writestring("  ");

        if (u->proc && u->proc->pid) {
            write_u64_padded((uint64_t)u->proc->pid, 3);
        } else {
            write_padded("-", 3);
        }
        console_writestring("  ");

        int pin = uthread_index_in_proc(u);
        if (pin > 0) {
            write_u64_padded((uint64_t)pin, 7);
        } else {
            write_padded("-", 7);
        }
        console_writestring("  ");

        write_u64_padded((uint64_t)u->uthread_id, 4);
        console_writestring("  ");
        write_padded(uthread_type_name(u->type), 6);
        console_writestring("  ");
        write_padded(uthread_state_name(u->state), 10);
        console_writestring("  ");
        write_u64_padded((uint64_t)u->priority, 4);
        console_writestring("    ");

        if (u->proc && u->proc->name[0]) {
            write_padded(u->proc->name, 14);
        } else if (u->lwkt && u->lwkt->name[0]) {
            write_padded(u->lwkt->name, 14);
        } else {
            write_padded("-", 14);
        }
        console_putchar('\n');
    }

    console_writestring("\nTotal: ");
    console_write_dec((uint64_t)count);
    console_writestring(" uthreads\n");
}
