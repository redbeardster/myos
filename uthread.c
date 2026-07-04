#include "uthread.h"

#include "msgport.h"
#include "proc.h"
#include "user.h"

#include <stddef.h>
#include <stdint.h>

#define MAX_UTHREADS MAX_THREADS

static struct uthread uthread_pool[MAX_UTHREADS];

static struct uthread *alloc_slot(void) {
    for (int i = 0; i < MAX_UTHREADS; i++) {
        if (uthread_pool[i].lwkt == NULL) {
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
    u->entry = NULL;
    u->arg = NULL;
    u->user_rip = 0;
    u->user_rsp = 0;
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

    user_enter(u->user_rip, u->user_rsp, &u->lwkt->saved_kernel_rsp);
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

struct uthread *uthread_spawn_in_proc(struct proc *p, uint64_t rip, uint64_t rsp, uint32_t priority) {
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

    char tname[32];
    tname[0] = 'u';
    int i = 1;
    uint32_t pid = p->pid;
    if (pid >= 10) {
        tname[i++] = '0' + (char)((pid / 10) % 10);
    }
    tname[i++] = '0' + (char)(pid % 10);
    if (p->uthread_count > 0) {
        tname[i++] = '.';
        tname[i++] = '0' + (char)((p->uthread_count + 1) % 10);
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
        uthread_cleanup(u);
    }

    lwkt_thread_exit();
}
