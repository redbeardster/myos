#ifndef UTHREAD_H
#define UTHREAD_H

#include <stdint.h>
#include "lwkt.h"

enum uthread_type {
    UTHREAD_KERNEL = 0,
    UTHREAD_USER = 1
};

enum uthread_state {
    UTHREAD_RUNNABLE = 0,
    UTHREAD_RUNNING = 1,
    UTHREAD_BLOCKED = 2,
    UTHREAD_ZOMBIE = 3
};

struct proc;

struct uthread {
    uint32_t slot;
    uint32_t uthread_id;
    enum uthread_type type;
    enum uthread_state state;
    uint32_t priority;
    void (*entry)(void *);
    void *arg;
    uint64_t user_rip;
    uint64_t user_rsp;
    uint64_t user_rdi;
    uint64_t user_syscall_ret;
    uint64_t user_rbx;
    uint64_t user_rbp;
    uint64_t user_r12;
    uint64_t user_r13;
    uint64_t user_r14;
    uint64_t user_r15;
    uint8_t user_regs_valid;
    uint64_t user_arg;
    uint64_t user_stack_base;
    int exit_code;
    struct uthread *join_waiter;
    struct proc *proc;
    struct lwkt_thread *lwkt;
    struct uthread *next_in_proc;
    struct uthread *run_next;
};

struct uthread *uthread_spawn(const char *name, void (*entry)(void *), void *arg, uint32_t priority);
struct uthread *uthread_spawn_in_proc(struct proc *p, uint64_t rip, uint64_t rsp,
                                      uint64_t arg, uint64_t stack_base, uint32_t priority);
int uthread_create_in_proc(struct proc *p, uint64_t rip, uint64_t arg, uint32_t priority);
int uthread_join(uint32_t uthread_id, int *exit_code_out);
int uthread_kill(uint32_t uthread_id);
struct uthread *uthread_lookup(uint32_t uthread_id);
struct uthread *uthread_current(void);
int uthread_ptr_valid(const struct uthread *u);
void uthread_exit(void);
void uthread_yield(void);
void uthread_list(void);

void uthread_reap_proc(struct proc *p);

uint32_t uthread_slot_of(const struct uthread *u);
int uthread_index_in_proc(const struct uthread *u);

#endif
