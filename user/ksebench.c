#include "myos_thread.h"
#include "myos_util.h"

extern unsigned long __exec_arg;

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

static void bench_str(const char *s) {
    myos_mutex_lock(MYOS_MUTEX_CONSOLE);
    myos_write_str(s);
    myos_mutex_unlock(MYOS_MUTEX_CONSOLE);
}

static void bench_char(char c) {
    myos_mutex_lock(MYOS_MUTEX_CONSOLE);
    myos_write_char(c);
    myos_mutex_unlock(MYOS_MUTEX_CONSOLE);
}

static void bench_dec(unsigned long n) {
    myos_mutex_lock(MYOS_MUTEX_CONSOLE);
    myos_write_dec(n);
    myos_mutex_unlock(MYOS_MUTEX_CONSOLE);
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
        bench_str("n/a");
        return;
    }
    unsigned long scaled = (num * 1000UL) / den;
    bench_dec(scaled / 1000);
    bench_char('.');
    unsigned long frac = scaled % 1000;
    if (frac < 100) {
        bench_char('0');
    }
    if (frac < 10) {
        bench_char('0');
    }
    bench_dec(frac);
}

typedef void (*bench_worker_fn)(uint64_t);

static int run_benchmark(const char *label, bench_worker_fn worker, unsigned long nworkers,
                         unsigned long iters, struct bench_result *out) {
    shared->counter = 0;
    shared->workers_done = 0;
    shared->iters = iters;

    bench_str("\n--- ksebench run: ");
    bench_str(label);
    bench_str(" ---\n");

    long tids[KSEBENCH_MAX_WORKERS];
    unsigned long t0 = (unsigned long)myos_ticks();

    for (unsigned long i = 0; i < nworkers; i++) {
        tids[i] = myos_thread_spawn(worker, i + 1);
        if (tids[i] < 0) {
            bench_str("ksebench: create failed ");
            bench_str(label);
            bench_str(" i=");
            bench_dec(i);
            bench_str(" rc=");
            bench_dec((unsigned long)(0 - tids[i]));
            bench_char('\n');
            return -2;
        }
    }

    for (unsigned long i = 0; i < nworkers; i++) {
        long rc = myos_thread_join(tids[i]);
        if (rc < 0) {
            bench_str("ksebench: join failed ");
            bench_str(label);
            bench_str(" rc=");
            bench_dec((unsigned long)(0 - rc));
            bench_char('\n');
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

    bench_str("counter=");
    bench_dec(out->counter);
    bench_str(" expected=");
    bench_dec(expected);
    bench_str(out->ok ? " OK\n" : " MISMATCH\n");

    bench_str("elapsed_ticks=");
    bench_dec(out->elapsed);
    bench_char('\n');

    bench_str("inc_per_tick=");
    write_ratio(out->counter, out->elapsed);
    bench_char('\n');

    bench_str("inc_per_tick_per_worker=");
    write_ratio(out->counter, out->elapsed * nworkers);
    bench_char('\n');

    return 0;
}

static void print_single_report(unsigned long nworkers, unsigned long iters,
                                const struct bench_result *r) {
    bench_str("\n=== ksebench report ===\n");
    bench_str("workers=");
    bench_dec(nworkers);
    bench_str(" iters=");
    bench_dec(iters);
    bench_str(" profile=mutex schedmode=KSE\n");

    bench_str("counter=");
    bench_dec(r->counter);
    bench_str(" expected=");
    bench_dec(r->expected);
    bench_str(r->ok ? " OK\n" : " MISMATCH\n");

    bench_str("workers_done=");
    bench_dec(r->workers_done);
    bench_char('\n');

    bench_str("elapsed_ticks=");
    bench_dec(r->elapsed);
    bench_char('\n');

    bench_str("inc_per_tick=");
    write_ratio(r->counter, r->elapsed);
    bench_char('\n');

    bench_str("inc_per_tick_per_worker=");
    write_ratio(r->counter, r->elapsed * nworkers);
    bench_char('\n');

    bench_str("ticks_per_1M_incs=");
    if (r->counter == 0) {
        bench_str("n/a\n");
    } else {
        write_ratio(r->elapsed * 1000000UL, r->counter);
        bench_char('\n');
    }
}

static void print_compare_report(unsigned long nworkers, unsigned long iters,
                                 const struct bench_result *mutex,
                                 const struct bench_result *parallel) {
    bench_str("\n=== ksebench compare (KSE) ===\n");
    bench_str("workers=");
    bench_dec(nworkers);
    bench_str(" iters=");
    bench_dec(iters);
    bench_char('\n');

    bench_str("mutex    elapsed_ticks=");
    bench_dec(mutex->elapsed);
    bench_str(" inc_per_tick=");
    write_ratio(mutex->counter, mutex->elapsed);
    bench_str(mutex->ok ? " OK\n" : " FAIL\n");

    bench_str("parallel elapsed_ticks=");
    bench_dec(parallel->elapsed);
    bench_str(" inc_per_tick=");
    write_ratio(parallel->counter, parallel->elapsed);
    bench_str(parallel->ok ? " OK\n" : " FAIL\n");

    bench_str("throughput_ratio parallel/mutex=");
    write_ratio(parallel->counter * mutex->elapsed, mutex->counter * parallel->elapsed);
    bench_str("x\n");

    bench_str("time_ratio mutex/parallel=");
    write_ratio(mutex->elapsed, parallel->elapsed);
    bench_str("x\n");

    if (parallel->counter * mutex->elapsed >= mutex->counter * parallel->elapsed) {
        bench_str("winner: parallel (");
        write_ratio((parallel->counter * mutex->elapsed) - (mutex->counter * parallel->elapsed),
                    mutex->counter * parallel->elapsed);
        bench_str("% faster throughput)\n");
    } else {
        bench_str("winner: mutex\n");
    }
}

int main(void) {
    for (int i = 0; i < 8; i++) {
        myos_yield();
    }

    unsigned long packed = __exec_arg;
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
        bench_str("ksebench: workers limit ");
        bench_dec(KSEBENCH_MAX_WORKERS);
        bench_char('\n');
        return 1;
    }

    shared = (struct bench_shared *)myos_alloc_page();
    if (!shared) {
        bench_str("ksebench: alloc failed\n");
        return 2;
    }

    if (compare) {
        bench_str("ksebench compare (mutex vs parallel, KSE): workers=");
        bench_dec(nworkers);
        bench_str(" iters=");
        bench_dec(iters);
        bench_char('\n');

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

    bench_str("ksebench: workers=");
    bench_dec(nworkers);
    bench_str(" iters=");
    bench_dec(iters);
    bench_str(" schedmode=KSE\n");

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
