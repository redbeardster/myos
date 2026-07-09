#ifndef PROC_H
#define PROC_H

#include <stdint.h>

#include "myos_abi.h"
#include "msgport.h"
#include "proc_mutex.h"

struct uthread;
struct lwkt_thread;

struct cap_entry {
    uint32_t target_lwkt_id;
    uint32_t rights;
    uint8_t in_use;
};

enum proc_state {
    PROC_RUNNING = 0,
    PROC_DEAD = 1
};

enum proc_sched_mode {
    PROC_SCHED_RUNNER = 0,
    PROC_SCHED_KSE = 1
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
    int read_wake;
    uint32_t sched_mode;
    int uthread_count;
    struct uthread *threads;
    struct uthread *main_thread;
    struct lwkt_thread *runner;
    struct uthread *current_uthread;
    struct uthread *run_queue;
    struct proc_mutex mutexes[PROC_MUTEX_MAX];
    struct cap_entry caps[MYOS_CAP_MAX];
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
void proc_sched_nudge(struct proc *p);
void proc_destroy(struct proc *p);
int proc_kill(uint32_t pid);
int proc_kill_name(const char *name);
int proc_set_sched_mode(struct proc *p, uint32_t mode);
int proc_get_sched_mode(struct proc *p);
void proc_kill_all(void);
void proc_kill_children(void);
void proc_list(void);
int proc_mutex_lock(uint32_t id);
int proc_mutex_unlock(uint32_t id);
int proc_cap_create_port(void);
int proc_cap_send(uint32_t cap_slot, uint32_t type, const void *data, uint32_t size);
int proc_cap_recv(uint32_t cap_slot, struct msg *out, int block);
int proc_cap_grant(uint32_t cap_slot, uint32_t target_pid, uint32_t rights);
int proc_cap_close(uint32_t cap_slot);
void proc_init(void);

#endif
