/*
 * Multi-uthread demo — run: exec threads.elf
 *
 * Full walkthrough: docs/THREADS_DEMO.md
 *
 * Two mutex slots:
 *   MYOS_MUTEX_DATA (0)    — protects *counter (preemption-safe)
 *   MYOS_MUTEX_CONSOLE (1) — serializes console lines
 *
 * Scheduling: uthreads run on LWKT; timer ~100Hz preempts LWKT
 * (see THREADS_DEMO.md §1). myos_yield() is cooperative only.
 */
#include "myos_thread.h"
#include "myos_util.h"

/* Shared heap page; all uthreads in this proc share the same CR3. */
static volatile unsigned long *counter;

/* One complete log line under CONSOLE mutex — avoids interleaved output. */
static void log_worker_line(unsigned long id, const char *msg) {
    myos_mutex_lock(MYOS_MUTEX_CONSOLE);
    myos_write_str("  worker ");
    myos_write_dec(id);
    myos_write_str(" ");
    myos_write_str(msg);
    myos_write_char('\n');
    myos_mutex_unlock(MYOS_MUTEX_CONSOLE);
}

/*
 * Worker entry: void worker(uint64_t arg) — arg in rdi (SysV).
 * Must call myos_exit(); plain return has no valid return address.
 */
void worker(uint64_t id) {
    log_worker_line(id, "start");

    for (int i = 0; i < 5; i++) {
        myos_mutex_lock(MYOS_MUTEX_DATA);
        (*counter)++;
        myos_mutex_unlock(MYOS_MUTEX_DATA);
        myos_yield(); /* optional; timer preempts anyway */
    }

    log_worker_line(id, "done");
    myos_exit((int)id); /* exit code returned by myos_thread_join */
}

int main(void) {
    counter = (volatile unsigned long *)myos_alloc_page();
    if (!counter) {
        myos_write_str("threads.elf: alloc failed\n");
        return 1;
    }
    *counter = 0;

    myos_write_str("threads.elf: spawning 2 workers (mutex 0=data, 1=console)\n");

    /* Returns uthread id (TID). t2 has HIGH prio → may run before t1. */
    long t1 = myos_thread_spawn(worker, 1);
    long t2 = myos_thread_spawn_prio(worker, 2, MYOS_THREAD_PRIO_HIGH);
    if (t1 < 0 || t2 < 0) {
        myos_write_str("thread create failed\n");
        return 1;
    }

    /* Blocks main uthread until each worker calls myos_exit. */
    long c1 = myos_thread_join(t1);
    long c2 = myos_thread_join(t2);

    myos_mutex_lock(MYOS_MUTEX_CONSOLE);
    myos_write_str("join: t1=");
    myos_write_dec((unsigned long)c1);
    myos_write_str(" t2=");
    myos_write_dec((unsigned long)c2);
    myos_write_str(" counter=");
    myos_write_dec(*counter); /* expect 10 = 2 workers * 5 increments */
    myos_write_char('\n');
    myos_mutex_unlock(MYOS_MUTEX_CONSOLE);

    myos_free_page((void *)counter);
    myos_write_str("main done\n");
    return 0; /* crt0 → SYS_EXIT for main uthread only */
}
