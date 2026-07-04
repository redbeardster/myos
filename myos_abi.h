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
#define MYOS_USER_STACK_TOP    0x0000000000700000ULL

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

#endif
