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
    proc_mutex_init_all(p->mutexes, PROC_MUTEX_MAX);
}

static const char *state_name(enum proc_state state) {
    return state == PROC_RUNNING ? "running" : "dead";
}

struct proc *proc_create(const char *name, uint64_t cr3, uint64_t entry,
                         uint64_t user_stack, int is_shell) {
    spin_lock(&proc_table_lock);
    struct proc *p = alloc_proc();
    if (!p) {
        spin_unlock(&proc_table_lock);
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
    spin_unlock(&proc_table_lock);
    return p;
}

struct proc *proc_current(void) {
    struct lwkt_thread *t = lwkt_curthread();
    if (!t || !t->uthread) {
        return NULL;
    }
    return t->uthread->proc;
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
    spin_lock(&proc_table_lock);
    u->next_in_proc = p->threads;
    p->threads = u;
    p->uthread_count++;
    if (!p->main_thread) {
        p->main_thread = u;
    }
    spin_unlock(&proc_table_lock);
}

void proc_detach_uthread(struct proc *p, struct uthread *u) {
    if (!p || !u) {
        return;
    }

    spin_lock(&proc_table_lock);
    struct uthread **slot = &p->threads;
    while (*slot) {
        if (*slot == u) {
            *slot = u->next_in_proc;
            u->next_in_proc = NULL;
            p->uthread_count--;
            if (p->main_thread == u) {
                p->main_thread = p->threads;
            }
            spin_unlock(&proc_table_lock);
            return;
        }
        slot = &(*slot)->next_in_proc;
    }
    spin_unlock(&proc_table_lock);
}

void proc_on_uthread_exit(struct proc *p, struct uthread *u) {
    if (!p || p->pid == 0) {
        return;
    }

    proc_detach_uthread(p, u);

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

    spin_lock(&proc_table_lock);

    if (p->cr3) {
        uint64_t cr3 = p->cr3;
        p->cr3 = 0;
        proc_destroy_aspace(cr3);
    }

    proc_reset_slot(p);
    spin_unlock(&proc_table_lock);
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
        ids[n++] = u->lwkt_id;
    }
    for (int i = 0; i < n; i++) {
        uthread_kill(ids[i]);
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

void proc_init(void) {
    spin_init(&proc_table_lock);
    for (int i = 0; i < MAX_PROCS; i++) {
        proc_reset_slot(&proc_table[i]);
    }
}
