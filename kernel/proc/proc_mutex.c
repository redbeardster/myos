#include "proc_mutex.h"

#include "lwkt.h"

void proc_mutex_init_all(struct proc_mutex *mutexes, int count) {
    if (!mutexes || count <= 0) {
        return;
    }
    if (count > PROC_MUTEX_MAX) {
        count = PROC_MUTEX_MAX;
    }
    for (int i = 0; i < count; i++) {
        token_init(&mutexes[i].lock);
    }
}

int proc_mutex_lock_slot(struct proc_mutex *mutexes, int count, uint32_t id) {
    if (!mutexes || id >= (uint32_t)count) {
        return -1;
    }
    token_lock(&mutexes[id].lock);
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
    token_unlock(&mutexes[id].lock);
    return 0;
}
