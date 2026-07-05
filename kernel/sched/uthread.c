#include "uthread.h"

#include "console.h"
#include "msgport.h"
#include "proc.h"
#include "user.h"

#include <stddef.h>
#include <stdint.h>

#define MAX_UTHREADS MAX_THREADS

#define MAX_LWKT_IDS 64

static struct uthread uthread_pool[MAX_UTHREADS];
static int join_exit_code[MAX_LWKT_IDS];
static int join_exit_valid[MAX_LWKT_IDS];

static struct uthread *alloc_slot(void) {
    for (int i = 0; i < MAX_UTHREADS; i++) {
        if (uthread_pool[i].lwkt == NULL) {
            uthread_pool[i].slot = (uint32_t)i;
            return &uthread_pool[i];
        }
    }
    return NULL;
}

static struct uthread *find_by_lwkt_id(uint32_t id) {
    for (int i = 0; i < MAX_UTHREADS; i++) {
        if (uthread_pool[i].lwkt && uthread_pool[i].lwkt_id == id) {
            return &uthread_pool[i];
        }
    }
    return NULL;
}

static void uthread_reset_slot(struct uthread *u) {
    if (!u) {
        return;
    }
    u->lwkt_id = 0;
    u->slot = 0;
    u->entry = NULL;
    u->arg = NULL;
    u->user_rip = 0;
    u->user_rsp = 0;
    u->user_arg = 0;
    u->user_stack_base = 0;
    u->exit_code = 0;
    u->join_waiter = NULL;
    u->proc = NULL;
    u->lwkt = NULL;
    u->next_in_proc = NULL;
    u->type = UTHREAD_KERNEL;
}

static void uthread_cleanup(struct uthread *u) {
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
        struct proc *p = u->proc;
        proc_on_uthread_exit(p, u);
        u->proc = NULL;
    }
    if (u->lwkt) {
        u->lwkt->uthread = NULL;
        u->lwkt->user_cr3 = 0;
    }
    uthread_reset_slot(u);
}

static void uthread_record_join(struct uthread *u) {
    if (!u || u->lwkt_id == 0 || u->lwkt_id >= MAX_LWKT_IDS) {
        return;
    }
    join_exit_code[u->lwkt_id] = u->exit_code;
    join_exit_valid[u->lwkt_id] = 1;
}

static void uthread_wakeup_joiner(struct uthread *u) {
    if (!u || !u->join_waiter) {
        return;
    }
    struct lwkt_thread *w = u->join_waiter;
    u->join_waiter = NULL;
    lwkt_unblock(w);
}

static void uthread_bind_lwkt(struct uthread *u, struct lwkt_thread *t) {
    u->lwkt_id = t->id;
    u->lwkt = t;
    t->uthread = u;
    if (u->type == UTHREAD_USER && u->proc) {
        t->user_cr3 = u->proc->cr3;
    } else {
        t->user_cr3 = 0;
    }
}

static void uthread_user_trampoline(void *arg) {
    struct uthread *u = (struct uthread *)arg;
    if (!u || !u->lwkt || !u->proc) {
        uthread_exit();
        return;
    }

    user_enter(u->user_rip, u->user_rsp, u->user_arg, &u->lwkt->saved_kernel_rsp);
    uthread_exit();
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
    u->proc = NULL;
    u->user_rip = 0;
    u->user_rsp = 0;

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
    if (!p || p->state != PROC_RUNNING || !p->cr3) {
        return NULL;
    }

    struct uthread *u = alloc_slot();
    if (!u) {
        return NULL;
    }

    u->entry = NULL;
    u->arg = NULL;
    u->type = UTHREAD_USER;
    u->proc = p;
    u->user_rip = rip;
    u->user_rsp = rsp;
    u->user_arg = arg;
    u->user_stack_base = stack_base;

    char tname[32];
    tname[0] = 'u';
    int i = 1;
    uint32_t pid = p->pid;
    if (pid >= 10) {
        tname[i++] = '0' + (char)((pid / 10) % 10);
    }
    tname[i++] = '0' + (char)(pid % 10);
    if (p->uthread_count > 0) {
        uint32_t tn = p->uthread_count + 1;
        tname[i++] = '.';
        if (tn >= 10) {
            tname[i++] = '0' + (char)((tn / 10) % 10);
        }
        tname[i++] = '0' + (char)(tn % 10);
    }
    tname[i] = '\0';

    struct lwkt_thread *t = lwkt_create(tname, uthread_user_trampoline, u, priority);
    if (!t) {
        u->proc = NULL;
        uthread_reset_slot(u);
        return NULL;
    }

    uthread_bind_lwkt(u, t);
    proc_attach_uthread(p, u);
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

    struct uthread *u = uthread_spawn_in_proc(p, rip, rsp, arg, base, priority);
    if (!u) {
        user_stack_free(p, base);
        return -3;
    }

    return (int)u->lwkt_id;
}

int uthread_kill(uint32_t lwkt_id) {
    struct uthread *u = find_by_lwkt_id(lwkt_id);
    if (!u) {
        return -1;
    }

    if (u->proc && proc_is_shell(u->proc)) {
        return -2;
    }

    int rc = lwkt_destroy(lwkt_id);
    if (rc == 0) {
        uthread_cleanup(u);
    }
    return rc;
}

struct uthread *uthread_lookup(uint32_t lwkt_id) {
    return find_by_lwkt_id(lwkt_id);
}

struct uthread *uthread_current(void) {
    struct lwkt_thread *t = lwkt_curthread();
    return t ? t->uthread : NULL;
}

void uthread_exit(void) {
    struct lwkt_thread *cur = lwkt_curthread();
    if (!cur) {
        return;
    }

    struct uthread *u = cur->uthread;
    if (u) {
        uthread_record_join(u);
        uthread_wakeup_joiner(u);
        uthread_cleanup(u);
    }

    lwkt_thread_exit();
}

int uthread_join(uint32_t lwkt_id, int *exit_code_out) {
    struct proc *p = proc_current();
    if (!p || lwkt_id == 0) {
        return -1;
    }

    if (lwkt_id < MAX_LWKT_IDS && join_exit_valid[lwkt_id]) {
        if (exit_code_out) {
            *exit_code_out = join_exit_code[lwkt_id];
        }
        join_exit_valid[lwkt_id] = 0;
        return 0;
    }

    struct uthread *u = find_by_lwkt_id(lwkt_id);
    if (!u || u->proc != p) {
        return -2;
    }

    struct lwkt_thread *self = lwkt_curthread();
    while (u && u->lwkt && u->lwkt->state != THREAD_TERMINATED) {
        u->join_waiter = self;
        lwkt_block();
        u->join_waiter = NULL;

        if (lwkt_id < MAX_LWKT_IDS && join_exit_valid[lwkt_id]) {
            if (exit_code_out) {
                *exit_code_out = join_exit_code[lwkt_id];
            }
            join_exit_valid[lwkt_id] = 0;
            return 0;
        }

        u = find_by_lwkt_id(lwkt_id);
        if (!u) {
            break;
        }
    }

    if (lwkt_id < MAX_LWKT_IDS && join_exit_valid[lwkt_id]) {
        if (exit_code_out) {
            *exit_code_out = join_exit_code[lwkt_id];
        }
        join_exit_valid[lwkt_id] = 0;
        return 0;
    }

    return -3;
}

uint32_t uthread_slot_of(const struct uthread *u) {
    if (!u || !u->lwkt) {
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

static const char *lwkt_state_name(struct lwkt_thread *t) {
    if (!t) {
        return "-";
    }
    switch (t->state) {
        case THREAD_READY: return "ready";
        case THREAD_RUNNING: return "running";
        case THREAD_BLOCKED: return "blocked";
        case THREAD_TERMINATED: return "dead";
        default: return "?";
    }
}

void uthread_list(void) {
    console_writestring("\nSlot  PID  #InProc  LWKT  Type    LWKT-State  Name\n");
    console_writestring("----  ---  -------  ----  ------  ----------  ----------\n");

    int count = 0;
    for (int i = 0; i < MAX_UTHREADS; i++) {
        struct uthread *u = &uthread_pool[i];
        if (!u->lwkt) {
            continue;
        }
        count++;

        console_write_dec(uthread_slot_of(u));
        console_writestring("  ");

        if (u->proc && u->proc->pid) {
            console_write_dec(u->proc->pid);
        } else {
            console_writestring("-");
        }
        console_writestring("  ");

        int pin = uthread_index_in_proc(u);
        if (pin > 0) {
            console_write_dec((uint64_t)pin);
        } else {
            console_putchar('-');
        }
        console_writestring("     ");

        console_write_dec(u->lwkt_id);
        console_writestring("  ");
        console_writestring(uthread_type_name(u->type));
        console_writestring("    ");
        console_writestring(lwkt_state_name(u->lwkt));
        console_writestring("    ");

        if (u->proc && u->proc->name[0]) {
            console_writestring(u->proc->name);
        } else if (u->lwkt && u->lwkt->name[0]) {
            console_writestring(u->lwkt->name);
        } else {
            console_writestring("-");
        }
        console_putchar('\n');
    }

    console_writestring("\nTotal: ");
    console_write_dec((uint64_t)count);
    console_writestring(" uthreads\n");
}
