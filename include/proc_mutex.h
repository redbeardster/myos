#ifndef PROC_MUTEX_H
#define PROC_MUTEX_H

#include <stdint.h>

#include "myos_abi.h"
#include "lwkt.h"
#include "spinlock.h"

#define PROC_MUTEX_MAX MYOS_PROC_MUTEX_MAX

struct proc_mutex {
    spinlock_t guard;
    int locked;
    struct lwkt_thread *waiters;
};

void proc_mutex_init_all(struct proc_mutex *mutexes, int count);
int proc_mutex_lock_slot(struct proc_mutex *mutexes, int count, uint32_t id);
int proc_mutex_unlock_slot(struct proc_mutex *mutexes, int count, uint32_t id);

#endif
