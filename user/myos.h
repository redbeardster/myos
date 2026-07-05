#ifndef MYOS_H
#define MYOS_H

#include <stdint.h>
#include "myos_abi.h"

typedef long myos_ssize_t;

static inline long myos_syscall4(long num, long a1, long a2, long a3, long a4) {
    long ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "D"(a1), "S"(a2), "d"(a3), "c"(a4)
        : "memory", "r11");
    return ret;
}

static inline long myos_exit(int code) {
    return myos_syscall4(MYOS_SYS_EXIT, code, 0, 0, 0);
}

static inline myos_ssize_t myos_write(int fd, const void *buf, unsigned long len) {
    return (myos_ssize_t)myos_syscall4(MYOS_SYS_WRITE, fd, (long)(uintptr_t)buf, (long)len, 0);
}

static inline long myos_read_char(void) {
    return myos_syscall4(MYOS_SYS_READ, 0, 0, 0, 0);
}

static inline long myos_yield(void) {
    return myos_syscall4(MYOS_SYS_YIELD, 0, 0, 0, 0);
}

static inline long myos_exec(const char *path) {
    return myos_syscall4(MYOS_SYS_EXEC, (long)(uintptr_t)path, 0, 0, 0);
}

static inline void *myos_alloc_page(void) {
    return (void *)(uintptr_t)myos_syscall4(MYOS_SYS_ALLOC, 0, 0, 0, 0);
}

static inline long myos_free_page(void *page) {
    return myos_syscall4(MYOS_SYS_FREE, (long)(uintptr_t)page, 0, 0, 0);
}

static inline long myos_ps(void) {
    return myos_syscall4(MYOS_SYS_PS, 0, 0, 0, 0);
}

static inline long myos_threads(void) {
    return myos_syscall4(MYOS_SYS_THREADS, 0, 0, 0, 0);
}

static inline long myos_uthreads(void) {
    return myos_syscall4(MYOS_SYS_UTHREADS, 0, 0, 0, 0);
}

static inline long myos_msgd_id(void) {
    return myos_syscall4(MYOS_SYS_MSGD_ID, 0, 0, 0, 0);
}

static inline long myos_msg_send(long to_id, const void *buf, unsigned long len) {
    return myos_syscall4(MYOS_SYS_MSG_SEND, to_id, (long)(uintptr_t)buf, (long)len, 0);
}

static inline long myos_msg_ping(void) {
    return myos_syscall4(MYOS_SYS_MSG_PING, 0, 0, 0, 0);
}

static inline long myos_thread_create(uintptr_t entry, uint64_t arg, long prio) {
    return myos_syscall4(MYOS_SYS_THREAD_CREATE, (long)entry, (long)arg, prio, 0);
}

static inline long myos_thread_join(long lwkt_id) {
    return myos_syscall4(MYOS_SYS_THREAD_JOIN, lwkt_id, 0, 0, 0);
}

static inline long myos_mutex_lock(unsigned long id) {
    return myos_syscall4(MYOS_SYS_MUTEX_LOCK, (long)id, 0, 0, 0);
}

static inline long myos_mutex_unlock(unsigned long id) {
    return myos_syscall4(MYOS_SYS_MUTEX_UNLOCK, (long)id, 0, 0, 0);
}

static inline long myos_cpus(void) {
    return myos_syscall4(MYOS_SYS_CPUS, 0, 0, 0, 0);
}

#endif
