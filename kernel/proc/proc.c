#include "proc.h"

#include "console.h"
#include "lwkt.h"
#include "myos_abi.h"
#include "proc_mutex.h"
#include "spinlock.h"
#include "uthread.h"
#include "vmm.h"

#include <stdint.h>

#define MAX_PROCS 8

static struct proc proc_table[MAX_PROCS];
static uint32_t next_pid = 1;
static spinlock_t proc_table_lock;

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

void proc_on_uthread_exit(struct proc *p, struct uthread *u) {
    if (!p || p->pid == 0) {
        return;
    }

    if (u) {
        proc_detach_uthread(p, u);
    }

    uthread_reap_proc(p);

    if (p->uthread_count > 0) {
        return;
    }

    if (!p->is_shell) {
        console_writestring("\nProcess ");
        console_write_dec(p->pid);
        console_writestring(" (");
        console_writestring(p->name);
        console_writestring(") exited\n");
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

void proc_destroy(struct proc *p) {
    if (!p || p->pid == 0) {
        return;
    }

    uint64_t irqf;
    spin_lock_irqsave(&proc_table_lock, &irqf);

    if (p->runner && p->runner->id) {
        struct lwkt_thread *cur = lwkt_curthread();
        uint32_t rid = p->runner->id;
        p->runner = NULL;
        spin_unlock_irqrestore(&proc_table_lock, irqf);
        if (cur != lwkt_find(rid)) {
            lwkt_destroy(rid);
        }
        spin_lock_irqsave(&proc_table_lock, &irqf);
    }

    if (p->cr3) {
        uint64_t cr3 = p->cr3;
        p->cr3 = 0;
        proc_destroy_aspace(cr3);
    }

    proc_reset_slot(p);
    spin_unlock_irqrestore(&proc_table_lock, irqf);
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

int proc_kill(uint32_t pid) {
    struct proc *p = find_proc(pid);
    if (!p || p->state != PROC_RUNNING) {
        return -1;
    }

    if (p->is_shell) {
        return -2;
    }

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

    if (p->runner && p->runner->id) {
        lwkt_destroy(p->runner->id);
        p->runner = NULL;
    }

    if (p->pid != 0) {
        proc_destroy(p);
    }
    return 0;
}

void proc_list(void) {
    console_writestring("\nPID  Name            State    CR3              Uthreads\n");
    console_writestring("---  --------------  -------  ---------------  -------\n");

    int count = 0;
    for (int i = 0; i < MAX_PROCS; i++) {
        struct proc *p = &proc_table[i];
        if (p->pid == 0) {
            continue;
        }
        count++;
        console_write_dec(p->pid);
        console_writestring("  ");
        console_writestring(p->name);
        console_writestring("  ");
        console_writestring(state_name(p->state));
        console_writestring("  ");
        console_write_hex(p->cr3);
        console_writestring("  ");
        console_write_dec((uint64_t)p->uthread_count);
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
