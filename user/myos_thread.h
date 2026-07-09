#ifndef MYOS_THREAD_H
#define MYOS_THREAD_H

#include <stdint.h>
#include "myos.h"

/*
 * MyOS user-thread helpers (uthreads in the current process).
 *
 * Entry point: void worker(uint64_t arg)  — SysV ABI, arg in rdi.
 * Worker must call myos_exit(); plain return crashes (no return address).
 *
 * Syscalls (myos.h): myos_thread_create, myos_thread_join,
 * myos_mutex_lock, myos_mutex_unlock
 */

typedef void (*myos_thread_fn)(uint64_t arg);

#define MYOS_THREAD_PRIO_HIGH   MYOS_PRIO_HIGH
#define MYOS_THREAD_PRIO_NORMAL MYOS_PRIO_NORMAL
#define MYOS_THREAD_PRIO_LOW    MYOS_PRIO_LOW

#define MYOS_MUTEX_DATA    0
#define MYOS_MUTEX_CONSOLE 1
#define MYOS_MUTEX_MAX     MYOS_PROC_MUTEX_MAX

static inline long myos_mutex_lock_default(void) {
    return myos_mutex_lock(MYOS_MUTEX_DEFAULT);
}

static inline long myos_mutex_unlock_default(void) {
    return myos_mutex_unlock(MYOS_MUTEX_DEFAULT);
}

static inline long myos_thread_spawn(myos_thread_fn fn, uint64_t arg) {
    long tid = myos_thread_create((uintptr_t)fn, arg, MYOS_THREAD_PRIO_NORMAL);
    if (tid >= 0) {
        myos_yield();
    }
    return tid;
}

static inline long myos_thread_spawn_prio(myos_thread_fn fn, uint64_t arg, long prio) {
    long tid = myos_thread_create((uintptr_t)fn, arg, prio);
    if (tid >= 0) {
        myos_yield();
    }
    return tid;
}

static inline long myos_thread_spawn_kse(myos_thread_fn fn, uint64_t arg, long prio) {
    return myos_thread_create_ex((uintptr_t)fn, arg, prio, MYOS_THREAD_F_KSE);
}

#endif
