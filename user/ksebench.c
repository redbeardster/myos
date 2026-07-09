#include "myos_thread.h"
#include "myos_util.h"

#define KSEBENCH_DEFAULT_WORKERS 6
#define KSEBENCH_DEFAULT_ITERS   200000
#define KSEBENCH_MAX_WORKERS     24

struct bench_shared {
    volatile unsigned long counter;
    volatile unsigned long workers_done;
    unsigned long iters;
};

struct bench_result {
    unsigned long counter;
    unsigned long elapsed;
    unsigned long workers_done;
    unsigned long expected;
    int ok;
};

static struct bench_shared *shared;

static unsigned long read_exec_config(void) {
    unsigned long packed;
    __asm__ volatile("mov %%rdi, %0" : "=r"(packed));
    return packed;
}

static void worker_mutex(uint64_t arg) {
    (void)arg;
    unsigned long iters = shared->iters;
    for (unsigned long i = 0; i < iters; i++) {
        myos_mutex_lock(MYOS_MUTEX_DATA);
        shared->counter++;
        myos_mutex_unlock(MYOS_MUTEX_DATA);
        if ((i & 0x3F) == 0) {
            myos_yield();
        }
    }
    myos_mutex_lock(MYOS_MUTEX_CONSOLE);
    shared->workers_done++;
    myos_mutex_unlock(MYOS_MUTEX_CONSOLE);
    myos_exit(0);
}

static void worker_parallel(uint64_t arg) {
    (void)arg;
    unsigned long iters = shared->iters;
    unsigned long local = 0;
    for (unsigned long i = 0; i < iters; i++) {
        local++;
        if ((i & 0x3F) == 0) {
            myos_yield();
        }
    }
    myos_mutex_lock(MYOS_MUTEX_DATA);
    shared->counter += local;
    shared->workers_done++;
    myos_mutex_unlock(MYOS_MUTEX_DATA);
    myos_exit(0);
}

static void write_ratio(unsigned long num, unsigned long den) {
    if (den == 0) {
        myos_write_str("n/a");
        return;
    }
    unsigned long scaled = (num * 1000UL) / den;
    myos_write_dec(scaled / 1000);
    myos_write_char('.');
    unsigned long frac = scaled % 1000;
    if (frac < 100) {
        myos_write_char('0');
    }
    if (frac < 10) {
        myos_write_char('0');
    }
    myos_write_dec(frac);
}

typedef void (*bench_worker_fn)(uint64_t);

static int run_benchmark(const char *label, bench_worker_fn worker, unsigned long nworkers,
                         unsigned long iters, struct bench_result *out) {
    shared->counter = 0;
    shared->workers_done = 0;
    shared->iters = iters;

    myos_write_str("\n--- ksebench run: ");
    myos_write_str(label);
    myos_write_str(" ---\n");

    long tids[KSEBENCH_MAX_WORKERS];
    unsigned long t0 = (unsigned long)myos_ticks();

    for (unsigned long i = 0; i < nworkers; i++) {
        tids[i] = myos_thread_spawn(worker, i + 1);
        if (tids[i] < 0) {
            myos_write_str("ksebench: create failed ");
            myos_write_str(label);
            myos_write_str(" i=");
            myos_write_dec(i);
            myos_write_str(" rc=");
            myos_write_dec((unsigned long)(0 - tids[i]));
            myos_write_char('\n');
            return -2;
        }
    }

    for (unsigned long i = 0; i < nworkers; i++) {
        long rc = myos_thread_join(tids[i]);
        if (rc < 0) {
            myos_write_str("ksebench: join failed ");
            myos_write_str(label);
            myos_write_str(" rc=");
            myos_write_dec((unsigned long)(0 - rc));
            myos_write_char('\n');
            return -3;
        }
    }

    unsigned long t1 = (unsigned long)myos_ticks();
    unsigned long expected = nworkers * iters;

    out->counter = shared->counter;
    out->elapsed = t1 - t0;
    out->workers_done = shared->workers_done;
    out->expected = expected;
    out->ok = (shared->counter == expected && shared->workers_done == nworkers);

    myos_write_str("counter=");
    myos_write_dec(out->counter);
    myos_write_str(" expected=");
    myos_write_dec(expected);
    myos_write_str(out->ok ? " OK\n" : " MISMATCH\n");

    myos_write_str("elapsed_ticks=");
    myos_write_dec(out->elapsed);
    myos_write_char('\n');

    myos_write_str("inc_per_tick=");
    write_ratio(out->counter, out->elapsed);
    myos_write_char('\n');

    myos_write_str("inc_per_tick_per_worker=");
    write_ratio(out->counter, out->elapsed * nworkers);
    myos_write_char('\n');

    return 0;
}

static void print_single_report(unsigned long nworkers, unsigned long iters,
                                const struct bench_result *r) {
    myos_write_str("\n=== ksebench report ===\n");
    myos_write_str("workers=");
    myos_write_dec(nworkers);
    myos_write_str(" iters=");
    myos_write_dec(iters);
    myos_write_str(" profile=mutex schedmode=KSE\n");

    myos_write_str("counter=");
    myos_write_dec(r->counter);
    myos_write_str(" expected=");
    myos_write_dec(r->expected);
    myos_write_str(r->ok ? " OK\n" : " MISMATCH\n");

    myos_write_str("workers_done=");
    myos_write_dec(r->workers_done);
    myos_write_char('\n');

    myos_write_str("elapsed_ticks=");
    myos_write_dec(r->elapsed);
    myos_write_char('\n');

    myos_write_str("inc_per_tick=");
    write_ratio(r->counter, r->elapsed);
    myos_write_char('\n');

    myos_write_str("inc_per_tick_per_worker=");
    write_ratio(r->counter, r->elapsed * nworkers);
    myos_write_char('\n');

    myos_write_str("ticks_per_1M_incs=");
    if (r->counter == 0) {
        myos_write_str("n/a\n");
    } else {
        write_ratio(r->elapsed * 1000000UL, r->counter);
        myos_write_char('\n');
    }

    myos_write_str("=== SMP balance (post-run) ===\n");
    myos_smp_balance();
}

static void print_compare_report(unsigned long nworkers, unsigned long iters,
                                 const struct bench_result *mutex,
                                 const struct bench_result *parallel) {
    myos_write_str("\n=== ksebench compare (KSE) ===\n");
    myos_write_str("workers=");
    myos_write_dec(nworkers);
    myos_write_str(" iters=");
    myos_write_dec(iters);
    myos_write_char('\n');

    myos_write_str("mutex    elapsed_ticks=");
    myos_write_dec(mutex->elapsed);
    myos_write_str(" inc_per_tick=");
    write_ratio(mutex->counter, mutex->elapsed);
    myos_write_str(mutex->ok ? " OK\n" : " FAIL\n");

    myos_write_str("parallel elapsed_ticks=");
    myos_write_dec(parallel->elapsed);
    myos_write_str(" inc_per_tick=");
    write_ratio(parallel->counter, parallel->elapsed);
    myos_write_str(parallel->ok ? " OK\n" : " FAIL\n");

    myos_write_str("throughput_ratio parallel/mutex=");
    write_ratio(parallel->counter * mutex->elapsed, mutex->counter * parallel->elapsed);
    myos_write_str("x\n");

    myos_write_str("time_ratio mutex/parallel=");
    write_ratio(mutex->elapsed, parallel->elapsed);
    myos_write_str("x\n");

    if (parallel->counter * mutex->elapsed >= mutex->counter * parallel->elapsed) {
        myos_write_str("winner: parallel (");
        write_ratio((parallel->counter * mutex->elapsed) - (mutex->counter * parallel->elapsed),
                    mutex->counter * parallel->elapsed);
        myos_write_str("% faster throughput)\n");
    } else {
        myos_write_str("winner: mutex\n");
    }

    myos_write_str("=== SMP balance (post-compare) ===\n");
    myos_smp_balance();
}

int main(void) {
    unsigned long packed = read_exec_config();
    unsigned long arg0 = (packed >> 32) & 0xFFFFFFFFUL;
    unsigned long iters = packed & 0xFFFFFFFFUL;
    unsigned long compare = (arg0 & MYOS_KSEBENCH_ARG_COMPARE) != 0;
    unsigned long nworkers = arg0 & 0xFFFFUL;

    if (nworkers == 0) {
        nworkers = KSEBENCH_DEFAULT_WORKERS;
    }
    if (iters == 0) {
        iters = KSEBENCH_DEFAULT_ITERS;
    }
    if (nworkers > KSEBENCH_MAX_WORKERS) {
        myos_write_str("ksebench: workers limit ");
        myos_write_dec(KSEBENCH_MAX_WORKERS);
        myos_write_char('\n');
        return 1;
    }

    shared = (struct bench_shared *)myos_alloc_page();
    if (!shared) {
        myos_write_str("ksebench: alloc failed\n");
        return 2;
    }

    if (compare) {
        myos_write_str("ksebench compare (mutex vs parallel, KSE): workers=");
        myos_write_dec(nworkers);
        myos_write_str(" iters=");
        myos_write_dec(iters);
        myos_write_char('\n');

        struct bench_result mutex;
        struct bench_result parallel;
        int rc = run_benchmark("mutex", worker_mutex, nworkers, iters, &mutex);
        if (rc != 0) {
            myos_free_page(shared);
            return 3;
        }
        rc = run_benchmark("parallel", worker_parallel, nworkers, iters, &parallel);
        if (rc != 0) {
            myos_free_page(shared);
            return 4;
        }

        print_compare_report(nworkers, iters, &mutex, &parallel);
        myos_free_page(shared);
        return (mutex.ok && parallel.ok) ? 0 : 5;
    }

    myos_write_str("ksebench: workers=");
    myos_write_dec(nworkers);
    myos_write_str(" iters=");
    myos_write_dec(iters);
    myos_write_str(" schedmode=KSE\n");

    struct bench_result result;
    int rc = run_benchmark("mutex", worker_mutex, nworkers, iters, &result);
    if (rc != 0) {
        myos_free_page(shared);
        return 6;
    }

    print_single_report(nworkers, iters, &result);
    myos_free_page(shared);
    return result.ok ? 0 : 7;
}
