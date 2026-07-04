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
    uint32_t lwkt_id;
    enum uthread_type type;
    void (*entry)(void *);
    void *arg;
    uint64_t user_rip;
    uint64_t user_rsp;
    struct proc *proc;
    struct lwkt_thread *lwkt;
    struct uthread *next_in_proc;
};

struct uthread *uthread_spawn(const char *name, void (*entry)(void *), void *arg, uint32_t priority);
struct uthread *uthread_spawn_in_proc(struct proc *p, uint64_t rip, uint64_t rsp, uint32_t priority);
int uthread_kill(uint32_t lwkt_id);
struct uthread *uthread_lookup(uint32_t lwkt_id);
struct uthread *uthread_current(void);
void uthread_exit(void);

#endif
