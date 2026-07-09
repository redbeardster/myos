#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>
#include "myos_abi.h"

#define SYS_EXIT     MYOS_SYS_EXIT
#define SYS_WRITE    MYOS_SYS_WRITE
#define SYS_YIELD    MYOS_SYS_YIELD
#define SYS_MSG_SEND MYOS_SYS_MSG_SEND
#define SYS_MSG_RECV MYOS_SYS_MSG_RECV
#define SYS_ALLOC    MYOS_SYS_ALLOC
#define SYS_FREE     MYOS_SYS_FREE
#define SYS_READ     MYOS_SYS_READ
#define SYS_EXEC     MYOS_SYS_EXEC
#define SYS_PS       MYOS_SYS_PS
#define SYS_THREADS  MYOS_SYS_THREADS
#define SYS_UTHREADS MYOS_SYS_UTHREADS
#define SYS_MSGD_ID  MYOS_SYS_MSGD_ID
#define SYS_MSG_PING MYOS_SYS_MSG_PING
#define SYS_THREAD_CREATE MYOS_SYS_THREAD_CREATE
#define SYS_THREAD_JOIN MYOS_SYS_THREAD_JOIN
#define SYS_MUTEX_LOCK MYOS_SYS_MUTEX_LOCK
#define SYS_MUTEX_UNLOCK MYOS_SYS_MUTEX_UNLOCK
#define SYS_CPUS       MYOS_SYS_CPUS
#define SYS_MSG_SEND_NAME MYOS_SYS_MSG_SEND_NAME
#define SYS_PORT_LOOKUP   MYOS_SYS_PORT_LOOKUP
#define SYS_MSG_PORTS     MYOS_SYS_MSG_PORTS
#define SYS_IPC_BUMP_MODE MYOS_SYS_IPC_BUMP_MODE

void syscall_init(void);
uint64_t syscall_dispatch(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3,
                          uint64_t user_rip, uint64_t user_rsp,
                          uint64_t user_rbx, uint64_t user_rbp,
                          uint64_t user_r12, uint64_t user_r13,
                          uint64_t user_r14, uint64_t user_r15);
uint64_t syscall_post_dispatch(uint64_t ret);

#endif
