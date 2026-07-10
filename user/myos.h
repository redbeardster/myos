#ifndef MYOS_H
#define MYOS_H

#include <stdint.h>
#include "myos_abi.h"

typedef long myos_ssize_t;

struct myos_msg {
    uint32_t from;
    uint32_t type;
    uint32_t size;
    uint8_t data[64];
};

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

static inline long myos_yield(void) {
    return myos_syscall4(MYOS_SYS_YIELD, 0, 0, 0, 0);
}

static inline long myos_read_char(void) {
    for (;;) {
        long ret = myos_syscall4(MYOS_SYS_READ, 0, 0, 0, 0);
        if (ret != MYOS_ERR_AGAIN) {
            return ret;
        }
        myos_yield();
    }
}

static inline long myos_exec(const char *path) {
    return myos_syscall4(MYOS_SYS_EXEC, (long)(uintptr_t)path, 0, 0, 0);
}

static inline long myos_exec_args(const char *path, unsigned long arg0, unsigned long arg1) {
    return myos_syscall4(MYOS_SYS_EXEC, (long)(uintptr_t)path, (long)arg0, (long)arg1, 0);
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

static inline long myos_msg_send_name(const char *port, const void *buf, unsigned long len) {
    return myos_syscall4(MYOS_SYS_MSG_SEND_NAME, (long)(uintptr_t)port,
                         (long)(uintptr_t)buf, (long)len, 0);
}

static inline long myos_port_lookup(const char *name) {
    return myos_syscall4(MYOS_SYS_PORT_LOOKUP, (long)(uintptr_t)name, 0, 0, 0);
}

static inline long myos_msg_ports(void) {
    return myos_syscall4(MYOS_SYS_MSG_PORTS, 0, 0, 0, 0);
}

static inline long myos_ipc_bump_mode(long mode) {
    return myos_syscall4(MYOS_SYS_IPC_BUMP_MODE, mode, 0, 0, 0);
}

static inline long myos_msg_ping_flags(long flags) {
    long rc = myos_syscall4(MYOS_SYS_MSG_PING, MYOS_MSG_PING_SEND | flags, 0, 0, 0);
    if (rc < 0 && rc != MYOS_ERR_AGAIN) {
        return rc;
    }
    for (;;) {
        rc = myos_syscall4(MYOS_SYS_MSG_PING, flags, 0, 0, 0);
        if (rc != MYOS_ERR_AGAIN) {
            return rc;
        }
        myos_yield();
    }
}

static inline long myos_msg_ping(void) {
    return myos_msg_ping_flags(0);
}

static inline long myos_thread_create(uintptr_t entry, uint64_t arg, long prio) {
    return myos_syscall4(MYOS_SYS_THREAD_CREATE, (long)entry, (long)arg, prio, 0);
}

static inline long myos_thread_create_ex(uintptr_t entry, uint64_t arg, long prio, long flags) {
    long packed = (prio & 0xFFFFL) | ((flags & 0xFFFFL) << 16);
    return myos_syscall4(MYOS_SYS_THREAD_CREATE_EX, (long)entry, (long)arg, packed, 0);
}

static inline long myos_thread_join(long uthread_id) {
    for (;;) {
        long ret = myos_syscall4(MYOS_SYS_THREAD_JOIN, uthread_id, 0, 0, 0);
        if (ret != MYOS_ERR_AGAIN) {
            return ret;
        }
    }
}

static inline long myos_mutex_lock(unsigned long id) {
    for (;;) {
        long ret = myos_syscall4(MYOS_SYS_MUTEX_LOCK, (long)id, 0, 0, 0);
        if (ret != MYOS_ERR_AGAIN) {
            return ret;
        }
    }
}

static inline long myos_mutex_unlock(unsigned long id) {
    return myos_syscall4(MYOS_SYS_MUTEX_UNLOCK, (long)id, 0, 0, 0);
}

static inline long myos_cpus(void) {
    return myos_syscall4(MYOS_SYS_CPUS, 0, 0, 0, 0);
}

static inline long myos_cap_create_port(void) {
    return myos_syscall4(MYOS_SYS_CAP_CREATE_PORT, 0, 0, 0, 0);
}

static inline long myos_cap_send(long cap_slot, const void *buf, unsigned long len) {
    return myos_syscall4(MYOS_SYS_CAP_SEND, cap_slot, (long)(uintptr_t)buf, (long)len, 0);
}

static inline long myos_cap_recv(long cap_slot, struct myos_msg *out, long block) {
    return myos_syscall4(MYOS_SYS_CAP_RECV, cap_slot, (long)(uintptr_t)out, block, 0);
}

static inline long myos_cap_grant(long cap_slot, long target_pid, long rights_mask) {
    return myos_syscall4(MYOS_SYS_CAP_GRANT, cap_slot, target_pid, rights_mask, 0);
}

static inline long myos_cap_close(long cap_slot) {
    return myos_syscall4(MYOS_SYS_CAP_CLOSE, cap_slot, 0, 0, 0);
}

static inline long myos_getpid(void) {
    return myos_syscall4(MYOS_SYS_GETPID, 0, 0, 0, 0);
}

static inline long myos_smp_balance(void) {
    return myos_syscall4(MYOS_SYS_SMP_BALANCE, 0, 0, 0, 0);
}

static inline long myos_kill(long pid) {
    for (;;) {
        long ret = myos_syscall4(MYOS_SYS_KILL, pid, 0, 0, 0);
        if (ret != MYOS_ERR_AGAIN) {
            return ret;
        }
        myos_yield();
    }
}

static inline long myos_killall(void) {
    return myos_syscall4(MYOS_SYS_KILLALL, 0, 0, 0, 0);
}

static inline long myos_killall_name(const char *name) {
    return myos_syscall4(MYOS_SYS_KILLALL_NAME, (long)(uintptr_t)name, 0, 0, 0);
}

static inline long myos_proc_set_sched_mode(long mode) {
    return myos_syscall4(MYOS_SYS_PROC_SET_SCHED_MODE, mode, 0, 0, 0);
}

static inline long myos_proc_get_sched_mode(void) {
    return myos_syscall4(MYOS_SYS_PROC_GET_SCHED_MODE, 0, 0, 0, 0);
}

static inline long myos_ticks(void) {
    return myos_syscall4(MYOS_SYS_TICKS, 0, 0, 0, 0);
}

#endif
