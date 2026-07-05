#include "proc_mutex.h"

#include "myos_abi.h"

void proc_mutex_init_all(struct proc_mutex *mutexes, int count) {
    if (!mutexes || count <= 0) {
        return;
    }
    if (count > PROC_MUTEX_MAX) {
        count = PROC_MUTEX_MAX;
    }
    for (int i = 0; i < count; i++) {
        spin_init(&mutexes[i].guard);
        mutexes[i].locked = 0;
        mutexes[i].waiters = NULL;
    }
}

static void wait_enqueue(struct proc_mutex *m, struct lwkt_thread *self) {
    self->wait_next = NULL;
    if (!m->waiters) {
        m->waiters = self;
        return;
    }
    struct lwkt_thread *tail = m->waiters;
    while (tail->wait_next) {
        tail = tail->wait_next;
    }
    tail->wait_next = self;
}

static struct lwkt_thread *wait_dequeue(struct proc_mutex *m) {
    struct lwkt_thread *w = m->waiters;
    if (!w) {
        return NULL;
    }
    m->waiters = w->wait_next;
    w->wait_next = NULL;
    return w;
}

int proc_mutex_lock_slot(struct proc_mutex *mutexes, int count, uint32_t id) {
    if (!mutexes || id >= (uint32_t)count) {
        return -1;
    }

    struct lwkt_thread *self = lwkt_curthread();
    if (!self) {
        return -1;
    }

    struct proc_mutex *m = &mutexes[id];
    for (;;) {
        spin_lock(&m->guard);
        if (!m->locked) {
            m->locked = 1;
            spin_unlock(&m->guard);
            return 0;
        }

        int already_queued = 0;
        for (struct lwkt_thread *w = m->waiters; w; w = w->wait_next) {
            if (w == self) {
                already_queued = 1;
                break;
            }
        }
        if (!already_queued) {
            wait_enqueue(m, self);
        }
        spin_unlock(&m->guard);
        lwkt_block();
    }
}

int proc_mutex_unlock_slot(struct proc_mutex *mutexes, int count, uint32_t id) {
    if (!mutexes || id >= (uint32_t)count) {
        return -1;
    }

    struct proc_mutex *m = &mutexes[id];
    struct lwkt_thread *wake = NULL;

    spin_lock(&m->guard);
    if (!m->locked) {
        spin_unlock(&m->guard);
        return -2;
    }
    m->locked = 0;
    wake = wait_dequeue(m);
    spin_unlock(&m->guard);

    if (wake) {
        lwkt_unblock(wake);
    }
    return 0;
}
