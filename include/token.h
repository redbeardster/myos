#ifndef TOKEN_H
#define TOKEN_H

#include "spinlock.h"

struct lwkt_thread;

/*
 * Sleepable exclusive token (DragonFly-inspired, simplified).
 *
 * Short metadata updates run under spinlock_t guard; contention sleeps
 * via lwkt_block() instead of busy-waiting. Not for IRQ handlers or
 * scheduler hot paths — use spin_lock_irqsave there.
 */
struct token {
    spinlock_t guard;
    struct lwkt_thread *holder;
    struct lwkt_thread *waiters;
};

void token_init(struct token *t);
void token_lock(struct token *t);
int token_trylock(struct token *t);
void token_unlock(struct token *t);
void token_drop_holder(struct token *t, struct lwkt_thread *owner);

/*
 * Sleepable shared token: many readers OR one writer (DragonFly-inspired).
 * Write-preferring: pending writers block new readers.
 * Not for IRQ handlers.
 */
struct token_shared {
    spinlock_t guard;
    int readers;
    struct lwkt_thread *writer;
    struct lwkt_thread *read_waiters;
    struct lwkt_thread *write_waiters;
};

void token_shared_init(struct token_shared *t);
void token_shared_read_lock(struct token_shared *t);
void token_shared_read_unlock(struct token_shared *t);
int  token_shared_read_trylock(struct token_shared *t);
void token_shared_write_lock(struct token_shared *t);
void token_shared_write_unlock(struct token_shared *t);
int  token_shared_write_trylock(struct token_shared *t);

/* Returns 0 on success; basic single-thread sanity checks. */
int token_shared_selftest(void);

/* Spawns LWKT workers; prints result when scheduler is running. */
void token_shared_mp_selftest_start(void);

#endif
