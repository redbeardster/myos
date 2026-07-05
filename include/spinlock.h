#ifndef SPINLOCK_H
#define SPINLOCK_H

#include <stdint.h>

typedef struct {
    volatile int locked;
} spinlock_t;

static inline void spin_init(spinlock_t *sl) {
    sl->locked = 0;
}

static inline void spin_lock(spinlock_t *sl) {
    while (__sync_lock_test_and_set(&sl->locked, 1)) {
        while (sl->locked) {
            __asm__ volatile("pause" ::: "memory");
        }
    }
}

static inline void spin_unlock(spinlock_t *sl) {
    __sync_lock_release(&sl->locked);
}

static inline int spin_trylock(spinlock_t *sl) {
    return __sync_lock_test_and_set(&sl->locked, 1) == 0;
}

static inline uint64_t cpu_irq_save(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0" : "=rm"(flags));
    __asm__ volatile("cli" ::: "memory");
    return flags;
}

static inline void cpu_irq_restore(uint64_t flags) {
    __asm__ volatile("push %0; popfq" :: "rm"(flags) : "memory", "cc");
}

static inline void spin_lock_irqsave(spinlock_t *sl, uint64_t *flags) {
    *flags = cpu_irq_save();
    spin_lock(sl);
}

static inline void spin_unlock_irqrestore(spinlock_t *sl, uint64_t flags) {
    spin_unlock(sl);
    cpu_irq_restore(flags);
}

#endif
