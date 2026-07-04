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

void syscall_init(void);
uint64_t syscall_dispatch(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3);

#endif
