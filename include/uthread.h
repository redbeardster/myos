#ifndef UTHREAD_H
#define UTHREAD_H

#include <stdint.h>
#include "lwkt.h"

enum uthread_type {
    UTHREAD_KERNEL = 0,
    UTHREAD_USER = 1
};

struct proc;

struct uthread {
    uint32_t slot;
    uint32_t lwkt_id;
    enum uthread_type type;
    void (*entry)(void *);
    void *arg;
    uint64_t user_rip;
    uint64_t user_rsp;
    uint64_t user_arg;
    uint64_t user_stack_base;
    int exit_code;
    struct lwkt_thread *join_waiter;
    struct proc *proc;
    struct lwkt_thread *lwkt;
    struct uthread *next_in_proc;
};

struct uthread *uthread_spawn(const char *name, void (*entry)(void *), void *arg, uint32_t priority);
struct uthread *uthread_spawn_in_proc(struct proc *p, uint64_t rip, uint64_t rsp,
                                      uint64_t arg, uint64_t stack_base, uint32_t priority);
int uthread_create_in_proc(struct proc *p, uint64_t rip, uint64_t arg, uint32_t priority);
int uthread_join(uint32_t lwkt_id, int *exit_code_out);
int uthread_kill(uint32_t lwkt_id);
struct uthread *uthread_lookup(uint32_t lwkt_id);
struct uthread *uthread_current(void);
void uthread_exit(void);
void uthread_list(void);

uint32_t uthread_slot_of(const struct uthread *u);
int uthread_index_in_proc(const struct uthread *u);

#endif
