#ifndef PROC_H
#define PROC_H

#include <stdint.h>

#include "proc_mutex.h"

struct uthread;
struct lwkt_thread;

enum proc_state {
    PROC_RUNNING = 0,
    PROC_DEAD = 1
};

struct proc {
    uint32_t pid;
    char name[32];
    enum proc_state state;
    uint64_t cr3;
    uint64_t entry;
    uint64_t user_stack;
    uint64_t heap_next;
    uint64_t stack_next;
    int is_shell;
    int uthread_count;
    struct uthread *threads;
    struct uthread *main_thread;
    struct lwkt_thread *runner;
    struct uthread *current_uthread;
    struct uthread *run_queue;
    struct proc_mutex mutexes[PROC_MUTEX_MAX];
};

struct proc *proc_create(const char *name, uint64_t cr3, uint64_t entry,
                         uint64_t user_stack, int is_shell);
struct proc *proc_find(uint32_t pid);
struct proc *proc_current(void);
int proc_is_shell(struct proc *p);
void proc_attach_uthread(struct proc *p, struct uthread *u);
void proc_detach_uthread(struct proc *p, struct uthread *u);
void proc_on_uthread_exit(struct proc *p, struct uthread *u);
int proc_start_runner(struct proc *p, uint32_t lwkt_priority);
void proc_runner_resched(struct proc *p);
void proc_destroy(struct proc *p);
int proc_kill(uint32_t pid);
void proc_kill_all(void);
void proc_kill_children(void);
void proc_list(void);
int proc_mutex_lock(uint32_t id);
int proc_mutex_unlock(uint32_t id);
void proc_init(void);

#endif
