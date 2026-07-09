#ifndef MYOS_ABI_H
#define MYOS_ABI_H

/*
 * MyOS user/kernel ABI (not Linux-compatible).
 *
 * Syscall: int 0x80
 *   rax = syscall number
 *   rdi, rsi, rdx, rcx = arguments
 *   return value in rax (negative = error)
 *
 * Each process has its own page tables (CR3). All processes share the
 * same virtual layout below; physical pages are isolated per process.
 */

#define MYOS_USER_BASE         0x0000000000400000ULL
#define MYOS_USER_HEAP_START   0x0000000000500000ULL
#define MYOS_USER_STACK_BASE   0x0000000000600000ULL
#define MYOS_USER_STACK_TOP    0x0000000000700000ULL
#define MYOS_USER_LIMIT        MYOS_USER_STACK_TOP

#define MYOS_SYS_EXIT     0
#define MYOS_SYS_WRITE    1
#define MYOS_SYS_YIELD    2
#define MYOS_SYS_MSG_SEND 3
#define MYOS_SYS_MSG_RECV 4
#define MYOS_SYS_ALLOC    5
#define MYOS_SYS_FREE     6
#define MYOS_SYS_READ     7
#define MYOS_SYS_EXEC     8
#define MYOS_SYS_PS       9
#define MYOS_SYS_THREADS  10
#define MYOS_SYS_UTHREADS 11
#define MYOS_SYS_MSGD_ID  12
#define MYOS_SYS_MSG_PING 13
#define MYOS_SYS_THREAD_CREATE 14
#define MYOS_SYS_THREAD_JOIN   15
#define MYOS_SYS_MUTEX_LOCK    16
#define MYOS_SYS_MUTEX_UNLOCK  17
#define MYOS_SYS_CPUS          18
#define MYOS_SYS_MSG_SEND_NAME 19
#define MYOS_SYS_PORT_LOOKUP   20
#define MYOS_SYS_MSG_PORTS     21
#define MYOS_SYS_IPC_BUMP_MODE 22
#define MYOS_SYS_CAP_CREATE_PORT 23
#define MYOS_SYS_CAP_SEND        24
#define MYOS_SYS_CAP_RECV        25
#define MYOS_SYS_CAP_GRANT       26
#define MYOS_SYS_CAP_CLOSE       27
#define MYOS_SYS_GETPID          28

#define MYOS_PROC_MUTEX_MAX    8

#define MYOS_MUTEX_DEFAULT     0

#define MYOS_PRIO_HIGH    2
#define MYOS_PRIO_NORMAL  8
#define MYOS_PRIO_LOW     15

#define MYOS_ERR_AGAIN    (-11)
#define MYOS_ERR_NOENT    (-2)

#define MYOS_MSG_PING_SEND  1
#define MYOS_MSG_PING_SILENT 2

#define MYOS_CAP_MAX 32
#define MYOS_CAP_RIGHT_SEND 0x1
#define MYOS_CAP_RIGHT_RECV 0x2

#endif
