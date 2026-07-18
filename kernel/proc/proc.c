#include "proc.h"

#include "console.h"
#include "interrupt.h"
#include "lwkt.h"
#include "myos_abi.h"
#include "proc_mutex.h"
#include "spinlock.h"
#include "uthread.h"
#include "vmm.h"

#include <stdint.h>

#define MAX_PROCS 16
#define PROC_KILL_WAIT_TICKS 500

static struct proc proc_table[MAX_PROCS];
static uint32_t next_pid = 1;
static spinlock_t proc_table_lock;
/* schedmode on the shell selects mode for subsequent exec children (shell stays KSE). */
static uint32_t exec_sched_mode = PROC_SCHED_KSE;

static int proc_alloc_cap_slot(struct proc *p) {
    if (!p) {
        return -1;
    }
    for (uint32_t i = 0; i < MYOS_CAP_MAX; i++) {
        if (!p->caps[i].in_use) {
            return (int)i;
        }
    }
    return -1;
}

static void strcpy_local(char *dst, const char *src) {
    while (*src) {
        *dst++ = *src++;
    }
    *dst = '\0';
}

static int str_eq_local(const char *a, const char *b) {
    if (!a || !b) {
        return 0;
    }
    while (*a && *b) {
        if (*a != *b) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static struct proc *alloc_proc(void) {
    for (int i = 0; i < MAX_PROCS; i++) {
        if (proc_table[i].pid == 0) {
            return &proc_table[i];
        }
    }
    return NULL;
}

static struct proc *find_proc(uint32_t pid) {
    for (int i = 0; i < MAX_PROCS; i++) {
        if (proc_table[i].pid == pid) {
            return &proc_table[i];
        }
    }
    return NULL;
}

static int pid_in_use(uint32_t pid) {
    return find_proc(pid) != NULL;
}

static uint32_t alloc_pid(void) {
    for (uint32_t pid = 2; pid < next_pid; pid++) {
        if (!pid_in_use(pid)) {
            return pid;
        }
    }
    return next_pid++;
}

static void proc_reset_slot(struct proc *p) {
    if (!p) {
        return;
    }
    p->pid = 0;
    p->name[0] = '\0';
    p->state = PROC_DEAD;
    p->cr3 = 0;
    p->entry = 0;
    p->user_stack = 0;
    p->heap_next = 0;
    p->stack_next = 0;
    p->is_shell = 0;
    p->read_wake = 0;
    p->sched_mode = PROC_SCHED_KSE;
    p->uthread_count = 0;
    p->threads = NULL;
    p->main_thread = NULL;
    p->runner = NULL;
    p->current_uthread = NULL;
    p->run_queue = NULL;
    proc_mutex_init_all(p->mutexes, PROC_MUTEX_MAX);
    for (uint32_t i = 0; i < MYOS_CAP_MAX; i++) {
        p->caps[i].in_use = 0;
        p->caps[i].target_lwkt_id = 0;
        p->caps[i].rights = 0;
    }
}

static const char *state_name(enum proc_state state) {
    return state == PROC_RUNNING ? "running" : "dead";
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
    static const char hex[] = "0123456789abcdef";
    if (v == 0) {
        buf[n++] = '-';
    } else {
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

struct proc *proc_create(const char *name, uint64_t cr3, uint64_t entry,
                         uint64_t user_stack, int is_shell) {
    uint64_t irqf;
    spin_lock_irqsave(&proc_table_lock, &irqf);
    struct proc *p = alloc_proc();
    if (!p) {
        spin_unlock_irqrestore(&proc_table_lock, irqf);
        return NULL;
    }

    p->pid = alloc_pid();
    if (name) {
        strcpy_local(p->name, name);
    } else {
        p->name[0] = 'p';
        p->name[1] = '0' + (char)(p->pid % 10);
        p->name[2] = '\0';
    }
    p->state = PROC_RUNNING;
    p->cr3 = cr3;
    p->entry = entry;
    p->user_stack = user_stack;
    p->heap_next = MYOS_USER_HEAP_START;
    p->stack_next = MYOS_USER_STACK_TOP;
    p->is_shell = is_shell;
    /* Shell always runs KSE (1:1 LWKT); children take exec_sched_mode. */
    p->sched_mode = is_shell ? PROC_SCHED_KSE : exec_sched_mode;
    p->uthread_count = 0;
    p->threads = NULL;
    p->main_thread = NULL;
    proc_mutex_init_all(p->mutexes, PROC_MUTEX_MAX);
    spin_unlock_irqrestore(&proc_table_lock, irqf);
    return p;
}

struct proc *proc_current(void) {
    struct lwkt_thread *t = lwkt_curthread();
    if (!t) {
        return NULL;
    }
    if (t->user_proc) {
        return t->user_proc;
    }
    if (t->uthread) {
        return t->uthread->proc;
    }
    return NULL;
}

int proc_is_shell(struct proc *p) {
    return p && p->is_shell;
}

struct proc *proc_find(uint32_t pid) {
    return find_proc(pid);
}

void proc_attach_uthread(struct proc *p, struct uthread *u) {
    if (!p || !u) {
        return;
    }
    uint64_t irqf;
    spin_lock_irqsave(&proc_table_lock, &irqf);
    u->next_in_proc = p->threads;
    p->threads = u;
    p->uthread_count++;
    if (!p->main_thread) {
        p->main_thread = u;
    }
    spin_unlock_irqrestore(&proc_table_lock, irqf);
}

void proc_detach_uthread(struct proc *p, struct uthread *u) {
    if (!p || !u) {
        return;
    }

    uint64_t irqf;
    spin_lock_irqsave(&proc_table_lock, &irqf);
    struct uthread **slot = &p->threads;
    while (*slot) {
        if (*slot == u) {
            *slot = u->next_in_proc;
            u->next_in_proc = NULL;
            p->uthread_count--;
            if (p->main_thread == u) {
                p->main_thread = p->threads;
            }
            spin_unlock_irqrestore(&proc_table_lock, irqf);
            return;
        }
        slot = &(*slot)->next_in_proc;
    }
    spin_unlock_irqrestore(&proc_table_lock, irqf);
}

static struct proc *proc_find_shell_locked(void) {
    for (int i = 0; i < MAX_PROCS; i++) {
        struct proc *s = &proc_table[i];
        if (s->pid != 0 && s->is_shell) {
            return s;
        }
    }
    return NULL;
}

static void proc_wake_shell(void) {
    uint64_t irqf;
    struct proc *shell_proc;
    struct lwkt_thread *shell_lwkt = NULL;

    spin_lock_irqsave(&proc_table_lock, &irqf);
    shell_proc = proc_find_shell_locked();
    if (shell_proc) {
        shell_proc->read_wake = 1;
        if (shell_proc->main_thread && shell_proc->main_thread->lwkt) {
            shell_lwkt = shell_proc->main_thread->lwkt;
        } else if (shell_proc->current_uthread && shell_proc->current_uthread->lwkt) {
            shell_lwkt = shell_proc->current_uthread->lwkt;
        } else if (shell_proc->runner) {
            shell_lwkt = shell_proc->runner;
        }
    }
    spin_unlock_irqrestore(&proc_table_lock, irqf);

    if (!shell_proc) {
        return;
    }

    console_writestring("\nMyOS> ");
    if (shell_lwkt) {
        lwkt_nudge(shell_lwkt);
    }
}

/*
 * UART has no IRQ in this port; drain depends on the shell polling. Kick the
 * shell when COM1 has bytes so FIFO overflow during busy SMP work is rarer.
 */
void proc_shell_serial_kick(void) {
    if (!serial_has_char()) {
        return;
    }

    uint64_t irqf;
    struct proc *shell_proc;
    struct lwkt_thread *shell_lwkt = NULL;

    spin_lock_irqsave(&proc_table_lock, &irqf);
    shell_proc = proc_find_shell_locked();
    if (shell_proc) {
        shell_proc->read_wake = 1;
        if (shell_proc->main_thread && shell_proc->main_thread->lwkt) {
            shell_lwkt = shell_proc->main_thread->lwkt;
        } else if (shell_proc->current_uthread && shell_proc->current_uthread->lwkt) {
            shell_lwkt = shell_proc->current_uthread->lwkt;
        } else if (shell_proc->runner) {
            shell_lwkt = shell_proc->runner;
        }
    }
    spin_unlock_irqrestore(&proc_table_lock, irqf);

    if (shell_lwkt) {
        lwkt_nudge(shell_lwkt);
    } else if (shell_proc) {
        lwkt_preempt_request();
    }
}

void proc_on_uthread_exit(struct proc *p, struct uthread *u) {
    if (!p || p->pid == 0) {
        return;
    }

    if (u) {
        proc_detach_uthread(p, u);
    }

    uthread_reap_proc(p);

    if (u && u->state == UTHREAD_ZOMBIE) {
        uthread_discard_zombie(u);
    }

    if (p->uthread_count > 0) {
        return;
    }

    if (!p->is_shell) {
        console_writestring("\nProcess ");
        console_write_dec(p->pid);
        console_writestring(" (");
        console_writestring(p->name);
        console_writestring(") exited\n");
        proc_wake_shell();
    }

    proc_destroy(p);
}

static void proc_destroy_aspace(uint64_t cr3) {
    if (!cr3 || cr3 == vmm_kernel_cr3()) {
        return;
    }

    if (vmm_cr3_active(cr3)) {
        struct lwkt_thread *cur = lwkt_curthread();
        if (cur) {
            cur->pending_cr3_destroy = cr3;
            return;
        }
    }

    vmm_aspace_destroy(cr3);
}

static void proc_request_kill_locked(struct proc *p,
                                     struct lwkt_thread **wake_list,
                                     int *wake_count,
                                     int wake_max,
                                     struct lwkt_thread **ipi_list,
                                     int *ipi_count,
                                     int ipi_max) {
    for (struct uthread *u = p->threads; u; u = u->next_in_proc) {
        if (u->lwkt && u->lwkt->id) {
            u->lwkt->pending_kill = 1;
            if (ipi_list && ipi_count && *ipi_count < ipi_max) {
                ipi_list[(*ipi_count)++] = u->lwkt;
            }
            if (u->lwkt->state == THREAD_BLOCKED && wake_list && wake_count &&
                *wake_count < wake_max) {
                wake_list[(*wake_count)++] = u->lwkt;
            }
        }
    }
    if (p->runner && p->runner->id) {
        p->runner->pending_kill = 1;
        if (ipi_list && ipi_count && *ipi_count < ipi_max) {
            ipi_list[(*ipi_count)++] = p->runner;
        }
        if (p->runner->state == THREAD_BLOCKED && wake_list && wake_count &&
            *wake_count < wake_max) {
            wake_list[(*wake_count)++] = p->runner;
        }
    }
}

void proc_destroy(struct proc *p) {
    if (!p || p->pid == 0) {
        return;
    }
    if (p->is_shell) {
        return;
    }

    uint64_t irqf;
    struct lwkt_thread *wake_list[8];
    struct lwkt_thread *ipi_list[MAX_THREADS];
    int wake_count = 0;
    int ipi_count = 0;

    spin_lock_irqsave(&proc_table_lock, &irqf);

    struct lwkt_thread *cur = lwkt_curthread();
    int on_proc_lwkt = cur && cur->user_proc == p;

    if (!on_proc_lwkt && (p->uthread_count > 0 || p->runner)) {
        proc_request_kill_locked(p, wake_list, &wake_count, 8, ipi_list, &ipi_count,
                                 MAX_THREADS);
        spin_unlock_irqrestore(&proc_table_lock, irqf);
        for (int i = 0; i < wake_count; i++) {
            lwkt_unblock(wake_list[i]);
        }
        lwkt_sched_ipi_threads(ipi_list, ipi_count);
        return;
    }

    if (p->uthread_count > 0) {
        spin_unlock_irqrestore(&proc_table_lock, irqf);
        return;
    }

    if (p->runner && cur != p->runner) {
        wake_count = 0;
        ipi_count = 0;
        proc_request_kill_locked(p, wake_list, &wake_count, 8, ipi_list, &ipi_count,
                                 MAX_THREADS);
        spin_unlock_irqrestore(&proc_table_lock, irqf);
        for (int i = 0; i < wake_count; i++) {
            lwkt_unblock(wake_list[i]);
        }
        lwkt_sched_ipi_threads(ipi_list, ipi_count);
        return;
    }

    if (p->runner) {
        p->runner = NULL;
    }

    uint64_t cr3 = 0;
    if (p->cr3) {
        cr3 = p->cr3;
        p->cr3 = 0;
    }

    proc_reset_slot(p);
    spin_unlock_irqrestore(&proc_table_lock, irqf);

    if (cr3) {
        proc_destroy_aspace(cr3);
    }
}

void proc_kill_children(void) {
    for (int i = 0; i < MAX_PROCS; i++) {
        struct proc *p = &proc_table[i];
        if (p->pid != 0 && p->state == PROC_RUNNING && !p->is_shell) {
            proc_kill(p->pid);
        }
    }
}

void proc_kill_all(void) {
    for (int i = 0; i < MAX_PROCS; i++) {
        if (proc_table[i].pid != 0 && proc_table[i].state == PROC_RUNNING) {
            proc_kill(proc_table[i].pid);
        }
    }
}

static int proc_is_dead(uint32_t pid) {
    struct proc *p = find_proc(pid);
    return !p || p->pid == 0;
}

static void proc_kill_nudge(uint32_t pid) {
    struct proc *p = find_proc(pid);
    if (p && p->state == PROC_RUNNING) {
        proc_sched_nudge(p);
    }
    __asm__ volatile("sti; hlt" ::: "memory");
}

static int proc_kill_wait_dead(uint32_t pid) {
    uint32_t start = (uint32_t)timer_get_ticks();
    while ((uint32_t)(timer_get_ticks() - start) < PROC_KILL_WAIT_TICKS) {
        if (proc_is_dead(pid)) {
            return 0;
        }
        proc_kill_nudge(pid);
    }
    return proc_is_dead(pid) ? 0 : -4;
}

static int proc_kill_request(uint32_t pid) {
    struct proc *p = find_proc(pid);
    if (!p || p->state != PROC_RUNNING) {
        return -1;
    }

    if (p->is_shell) {
        return -2;
    }

    struct lwkt_thread *cur = lwkt_curthread();
    if (cur && cur->user_proc == p) {
        uint32_t ids[MAX_THREADS];
        int n = 0;
        for (struct uthread *u = p->threads; u && n < MAX_THREADS; u = u->next_in_proc) {
            if (u->uthread_id) {
                ids[n++] = u->uthread_id;
            }
        }
        for (int i = 0; i < n; i++) {
            uthread_kill(ids[i]);
        }

        if (p->pid != 0) {
            proc_destroy(p);
        }
        return 0;
    }

    proc_destroy(p);
    return 0;
}

int proc_kill(uint32_t pid) {
    int rc = proc_kill_request(pid);
    if (rc != 0) {
        return rc;
    }
    if (proc_is_dead(pid)) {
        return 0;
    }
  /*
   * From int 0x80 the caller LWKT stays RUNNING across sti;hlt — victims on
   * the same CPU never run. Return EAGAIN so userland retries after yield.
   */
    if (lwkt_in_usersyscall()) {
        return MYOS_ERR_AGAIN;
    }
    return proc_kill_wait_dead(pid);
}

int proc_kill_name(const char *name) {
    if (!name || name[0] == '\0') {
        return -1;
    }

    uint32_t pids[MAX_PROCS];
    int count = 0;

    uint64_t irqf;
    spin_lock_irqsave(&proc_table_lock, &irqf);
    for (int i = 0; i < MAX_PROCS; i++) {
        struct proc *p = &proc_table[i];
        if (p->pid == 0 || p->state != PROC_RUNNING || p->is_shell) {
            continue;
        }
        if (!str_eq_local(p->name, name)) {
            continue;
        }
        if (count < MAX_PROCS) {
            pids[count++] = p->pid;
        }
    }
    spin_unlock_irqrestore(&proc_table_lock, irqf);

    if (count == 0) {
        return -2;
    }

    int killed = 0;
    int failed = 0;
    for (int i = 0; i < count; i++) {
        int rc = proc_kill(pids[i]);
        if (rc == 0) {
            killed++;
        } else if (rc != -1) {
            failed++;
        }
    }
    if (killed > 0) {
        return killed;
    }
    return failed > 0 ? -3 : -2;
}

int proc_set_sched_mode(struct proc *p, uint32_t mode) {
    if (!p) {
        return -1;
    }
    if (mode != PROC_SCHED_RUNNER && mode != PROC_SCHED_KSE) {
        return -2;
    }
    if (p->is_shell) {
        /* Preference for exec children only — do not retarget the live shell. */
        exec_sched_mode = mode;
        return 0;
    }
    p->sched_mode = mode;
    return 0;
}

int proc_get_sched_mode(struct proc *p) {
    if (!p) {
        return -1;
    }
    if (p->is_shell) {
        return (int)exec_sched_mode;
    }
    return (int)p->sched_mode;
}

void proc_list(void) {
    console_writestring("\nPID  Name            State    CR3              Uthreads\n");
    console_writestring("---  --------------  -------  ---------------  --------\n");

    int count = 0;
    for (int i = 0; i < MAX_PROCS; i++) {
        struct proc *p = &proc_table[i];
        if (p->pid == 0) {
            continue;
        }
        count++;
        write_u64_padded((uint64_t)p->pid, 3);
        console_writestring("  ");
        write_padded(p->name, 14);
        console_writestring("  ");
        write_padded(state_name(p->state), 7);
        console_writestring("  ");
        write_hex_padded(p->cr3, 15);
        console_writestring("  ");
        write_u64_padded((uint64_t)p->uthread_count, 8);
        console_putchar('\n');
    }

    console_writestring("\nTotal: ");
    console_write_dec((uint64_t)count);
    console_writestring(" processes\n");
}

int proc_mutex_lock(uint32_t id) {
    struct proc *p = proc_current();
    if (!p) {
        return -1;
    }
    return proc_mutex_lock_slot(p->mutexes, PROC_MUTEX_MAX, id);
}

int proc_mutex_unlock(uint32_t id) {
    struct proc *p = proc_current();
    if (!p) {
        return -1;
    }
    return proc_mutex_unlock_slot(p->mutexes, PROC_MUTEX_MAX, id);
}

int proc_cap_create_port(void) {
    struct proc *p = proc_current();
    struct lwkt_thread *cur = lwkt_curthread();
    if (!p || !cur || cur->id == 0) {
        return -1;
    }

    uint64_t irqf;
    spin_lock_irqsave(&proc_table_lock, &irqf);
    int slot = proc_alloc_cap_slot(p);
    if (slot >= 0) {
        p->caps[slot].in_use = 1;
        p->caps[slot].target_lwkt_id = cur->id;
        p->caps[slot].rights = MYOS_CAP_RIGHT_SEND | MYOS_CAP_RIGHT_RECV;
    }
    spin_unlock_irqrestore(&proc_table_lock, irqf);
    return slot;
}

int proc_cap_send(uint32_t cap_slot, uint32_t type, const void *data, uint32_t size) {
    struct proc *p = proc_current();
    if (!p || cap_slot >= MYOS_CAP_MAX) {
        return -1;
    }

    uint32_t target = 0;
    uint64_t irqf;
    spin_lock_irqsave(&proc_table_lock, &irqf);
    struct cap_entry *c = &p->caps[cap_slot];
    if (!c->in_use || (c->rights & MYOS_CAP_RIGHT_SEND) == 0) {
        spin_unlock_irqrestore(&proc_table_lock, irqf);
        return -2;
    }
    target = c->target_lwkt_id;
    spin_unlock_irqrestore(&proc_table_lock, irqf);

    return msg_send(target, type, data, size);
}

int proc_cap_recv(uint32_t cap_slot, struct msg *out, int block) {
    struct proc *p = proc_current();
    struct lwkt_thread *cur = lwkt_curthread();
    if (!p || !cur || cap_slot >= MYOS_CAP_MAX) {
        return -1;
    }

    uint32_t target = 0;
    uint64_t irqf;
    spin_lock_irqsave(&proc_table_lock, &irqf);
    struct cap_entry *c = &p->caps[cap_slot];
    if (!c->in_use || (c->rights & MYOS_CAP_RIGHT_RECV) == 0) {
        spin_unlock_irqrestore(&proc_table_lock, irqf);
        return -2;
    }
    target = c->target_lwkt_id;
    spin_unlock_irqrestore(&proc_table_lock, irqf);

    if (target != cur->id) {
        return -3;
    }
    return msg_receive(out, block);
}

int proc_cap_grant(uint32_t cap_slot, uint32_t target_pid, uint32_t rights) {
    struct proc *p = proc_current();
    if (!p || cap_slot >= MYOS_CAP_MAX || target_pid == 0) {
        return -1;
    }
    rights &= (MYOS_CAP_RIGHT_SEND | MYOS_CAP_RIGHT_RECV);
    if (rights == 0) {
        return -2;
    }

    uint64_t irqf;
    spin_lock_irqsave(&proc_table_lock, &irqf);
    struct cap_entry *src = &p->caps[cap_slot];
    if (!src->in_use) {
        spin_unlock_irqrestore(&proc_table_lock, irqf);
        return -3;
    }
    if ((src->rights & rights) != rights) {
        spin_unlock_irqrestore(&proc_table_lock, irqf);
        return -4;
    }
    struct proc *dst = find_proc(target_pid);
    if (!dst || dst->state != PROC_RUNNING) {
        spin_unlock_irqrestore(&proc_table_lock, irqf);
        return -5;
    }
    int dst_slot = proc_alloc_cap_slot(dst);
    if (dst_slot < 0) {
        spin_unlock_irqrestore(&proc_table_lock, irqf);
        return -6;
    }
    dst->caps[dst_slot].in_use = 1;
    dst->caps[dst_slot].target_lwkt_id = src->target_lwkt_id;
    dst->caps[dst_slot].rights = rights;
    spin_unlock_irqrestore(&proc_table_lock, irqf);
    return dst_slot;
}

int proc_cap_close(uint32_t cap_slot) {
    struct proc *p = proc_current();
    if (!p || cap_slot >= MYOS_CAP_MAX) {
        return -1;
    }

    uint64_t irqf;
    spin_lock_irqsave(&proc_table_lock, &irqf);
    struct cap_entry *c = &p->caps[cap_slot];
    if (!c->in_use) {
        spin_unlock_irqrestore(&proc_table_lock, irqf);
        return -2;
    }
    c->in_use = 0;
    c->target_lwkt_id = 0;
    c->rights = 0;
    spin_unlock_irqrestore(&proc_table_lock, irqf);
    return 0;
}

void proc_init(void) {
    spin_init(&proc_table_lock);
    for (int i = 0; i < MAX_PROCS; i++) {
        proc_reset_slot(&proc_table[i]);
    }
}
