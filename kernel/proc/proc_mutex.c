#include "proc_mutex.h"

#include "lwkt.h"
#include "myos_abi.h"
#include "proc.h"
#include "uthread.h"

void proc_mutex_init_all(struct proc_mutex *mutexes, int count) {
    if (!mutexes || count <= 0) {
        return;
    }
    if (count > PROC_MUTEX_MAX) {
        count = PROC_MUTEX_MAX;
    }
    for (int i = 0; i < count; i++) {
        token_init(&mutexes[i].lock);
        mutexes[i].uthread_holder = NULL;
    }
}

int proc_mutex_lock_slot(struct proc_mutex *mutexes, int count, uint32_t id) {
    if (!mutexes || id >= (uint32_t)count) {
        return -1;
    }

    struct token *tok = &mutexes[id].lock;
    struct proc *p = proc_current();
    struct uthread *u = uthread_current();
    struct lwkt_thread *self = lwkt_curthread();

    if (tok->holder == self && mutexes[id].uthread_holder != NULL &&
        mutexes[id].uthread_holder != u) {
        if (p && p->runner == self && u && lwkt_in_usersyscall()) {
            uthread_yield();
            return MYOS_ERR_AGAIN;
        }
        token_lock(tok);
        mutexes[id].uthread_holder = u;
        return 0;
    }

    if (token_trylock(tok)) {
        mutexes[id].uthread_holder = u;
        return 0;
    }

    if (p && p->runner == self && u && lwkt_in_usersyscall()) {
        uthread_yield();
        return MYOS_ERR_AGAIN;
    }

    token_lock(tok);
    return 0;
}

int proc_mutex_unlock_slot(struct proc_mutex *mutexes, int count, uint32_t id) {
    if (!mutexes || id >= (uint32_t)count) {
        return -1;
    }

    struct lwkt_thread *self = lwkt_curthread();
    if (!self || mutexes[id].lock.holder != self) {
        return -2;
    }
    mutexes[id].uthread_holder = NULL;
    token_unlock(&mutexes[id].lock);
    proc_runner_resched(proc_current());
    return 0;
}

void proc_mutex_abandon_all(struct proc_mutex *mutexes, int count, struct uthread *u) {
    struct lwkt_thread *runner = u && u->proc ? u->proc->runner : NULL;
    if (!mutexes || !runner) {
        return;
    }
    if (count > PROC_MUTEX_MAX) {
        count = PROC_MUTEX_MAX;
    }
    for (int i = 0; i < count; i++) {
        if (mutexes[i].uthread_holder == u) {
            token_drop_holder(&mutexes[i].lock, runner);
            mutexes[i].uthread_holder = NULL;
        }
    }
}
