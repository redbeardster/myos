#ifndef PROC_MUTEX_H
#define PROC_MUTEX_H

#include <stdint.h>

#include "myos_abi.h"
#include "token.h"

struct uthread;

#define PROC_MUTEX_MAX MYOS_PROC_MUTEX_MAX

struct proc_mutex {
    struct token lock;
    struct uthread *uthread_holder;
};

void proc_mutex_init_all(struct proc_mutex *mutexes, int count);
int proc_mutex_lock_slot(struct proc_mutex *mutexes, int count, uint32_t id);
int proc_mutex_unlock_slot(struct proc_mutex *mutexes, int count, uint32_t id);
void proc_mutex_abandon_all(struct proc_mutex *mutexes, int count, struct uthread *u);

#endif
