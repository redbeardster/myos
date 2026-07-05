#include "token.h"

#include "console.h"
#include "lwkt.h"

static void wait_enqueue(struct token *t, struct lwkt_thread *self) {
    self->wait_next = NULL;
    if (!t->waiters) {
        t->waiters = self;
        return;
    }
    struct lwkt_thread *tail = t->waiters;
    while (tail->wait_next) {
        tail = tail->wait_next;
    }
    tail->wait_next = self;
}

static struct lwkt_thread *wait_dequeue(struct token *t) {
    struct lwkt_thread *w = t->waiters;
    if (!w) {
        return NULL;
    }
    t->waiters = w->wait_next;
    w->wait_next = NULL;
    return w;
}

void token_init(struct token *t) {
    if (!t) {
        return;
    }
    spin_init(&t->guard);
    t->holder = NULL;
    t->waiters = NULL;
}

void token_lock(struct token *t) {
    if (!t) {
        return;
    }

    struct lwkt_thread *self = lwkt_curthread();
    if (!self) {
        return;
    }

    for (;;) {
        spin_lock(&t->guard);
        if (!t->holder) {
            t->holder = self;
            spin_unlock(&t->guard);
            return;
        }
        if (t->holder == self) {
            spin_unlock(&t->guard);
            return;
        }

        int already = 0;
        for (struct lwkt_thread *w = t->waiters; w; w = w->wait_next) {
            if (w == self) {
                already = 1;
                break;
            }
        }
        if (!already) {
            wait_enqueue(t, self);
        }
        spin_unlock(&t->guard);
        lwkt_block();
    }
}

int token_trylock(struct token *t) {
    if (!t) {
        return 0;
    }

    struct lwkt_thread *self = lwkt_curthread();
    if (!self) {
        return 0;
    }

    spin_lock(&t->guard);
    if (t->holder) {
        spin_unlock(&t->guard);
        return 0;
    }
    t->holder = self;
    spin_unlock(&t->guard);
    return 1;
}

void token_drop_holder(struct token *t, struct lwkt_thread *owner) {
    if (!t || !owner) {
        return;
    }

    struct lwkt_thread *wake = NULL;
    spin_lock(&t->guard);
    if (t->holder == owner) {
        t->holder = NULL;
        wake = wait_dequeue(t);
    }
    spin_unlock(&t->guard);

    if (wake) {
        lwkt_unblock(wake);
    }
}

void token_unlock(struct token *t) {
    if (!t) {
        return;
    }

    struct lwkt_thread *self = lwkt_curthread();
    struct lwkt_thread *wake = NULL;

    spin_lock(&t->guard);
    if (t->holder != self) {
        spin_unlock(&t->guard);
        return;
    }
    t->holder = NULL;
    wake = wait_dequeue(t);
    spin_unlock(&t->guard);

    if (wake) {
        lwkt_unblock(wake);
    }
}

/* --- token_shared (readers / writer) --- */

static void shared_wait_enqueue(struct lwkt_thread **head, struct lwkt_thread *self) {
    self->wait_next = NULL;
    if (!*head) {
        *head = self;
        return;
    }
    struct lwkt_thread *tail = *head;
    while (tail->wait_next) {
        tail = tail->wait_next;
    }
    tail->wait_next = self;
}

static struct lwkt_thread *shared_wait_dequeue(struct lwkt_thread **head) {
    struct lwkt_thread *w = *head;
    if (!w) {
        return NULL;
    }
    *head = w->wait_next;
    w->wait_next = NULL;
    return w;
}

static int shared_wait_contains(struct lwkt_thread **head, struct lwkt_thread *self) {
    for (struct lwkt_thread *w = *head; w; w = w->wait_next) {
        if (w == self) {
            return 1;
        }
    }
    return 0;
}

static void shared_wake_all(struct lwkt_thread **head) {
    struct lwkt_thread *w;
    while ((w = shared_wait_dequeue(head)) != NULL) {
        lwkt_unblock(w);
    }
}

void token_shared_init(struct token_shared *t) {
    if (!t) {
        return;
    }
    spin_init(&t->guard);
    t->readers = 0;
    t->writer = NULL;
    t->read_waiters = NULL;
    t->write_waiters = NULL;
}

void token_shared_read_lock(struct token_shared *t) {
    if (!t) {
        return;
    }

    struct lwkt_thread *self = lwkt_curthread();
    if (!self) {
        return;
    }

    for (;;) {
        spin_lock(&t->guard);
        if (!t->writer && !t->write_waiters) {
            t->readers++;
            spin_unlock(&t->guard);
            return;
        }
        if (!shared_wait_contains(&t->read_waiters, self)) {
            shared_wait_enqueue(&t->read_waiters, self);
        }
        spin_unlock(&t->guard);
        lwkt_block();
    }
}

int token_shared_read_trylock(struct token_shared *t) {
    if (!t) {
        return 0;
    }

    spin_lock(&t->guard);
    if (t->writer || t->write_waiters) {
        spin_unlock(&t->guard);
        return 0;
    }
    t->readers++;
    spin_unlock(&t->guard);
    return 1;
}

void token_shared_read_unlock(struct token_shared *t) {
    if (!t) {
        return;
    }

    struct lwkt_thread *wake = NULL;
    spin_lock(&t->guard);
    if (t->readers > 0) {
        t->readers--;
    }
    if (t->readers == 0 && t->write_waiters) {
        wake = shared_wait_dequeue(&t->write_waiters);
    }
    spin_unlock(&t->guard);

    if (wake) {
        lwkt_unblock(wake);
    }
}

void token_shared_write_lock(struct token_shared *t) {
    if (!t) {
        return;
    }

    struct lwkt_thread *self = lwkt_curthread();
    if (!self) {
        return;
    }

    for (;;) {
        spin_lock(&t->guard);
        if (!t->writer && t->readers == 0) {
            t->writer = self;
            spin_unlock(&t->guard);
            return;
        }
        if (t->writer == self) {
            spin_unlock(&t->guard);
            return;
        }
        if (!shared_wait_contains(&t->write_waiters, self)) {
            shared_wait_enqueue(&t->write_waiters, self);
        }
        spin_unlock(&t->guard);
        lwkt_block();
    }
}

int token_shared_write_trylock(struct token_shared *t) {
    if (!t) {
        return 0;
    }

    struct lwkt_thread *self = lwkt_curthread();
    if (!self) {
        return 0;
    }

    spin_lock(&t->guard);
    if (t->writer || t->readers > 0) {
        spin_unlock(&t->guard);
        return 0;
    }
    t->writer = self;
    spin_unlock(&t->guard);
    return 1;
}

void token_shared_write_unlock(struct token_shared *t) {
    if (!t) {
        return;
    }

    struct lwkt_thread *self = lwkt_curthread();
    struct lwkt_thread *wake_writer = NULL;

    spin_lock(&t->guard);
    if (t->writer != self) {
        spin_unlock(&t->guard);
        return;
    }
    t->writer = NULL;

    if (t->write_waiters) {
        wake_writer = shared_wait_dequeue(&t->write_waiters);
    }
    spin_unlock(&t->guard);

    if (wake_writer) {
        lwkt_unblock(wake_writer);
        return;
    }

    shared_wake_all(&t->read_waiters);
}

static struct token_shared selftest_ts;
static volatile int selftest_counter;
static volatile int selftest_read_ops;
static volatile int selftest_write_ops;
static volatile int selftest_workers_done;

static void selftest_checker(void *arg);

static void selftest_reader(void *arg) {
    (void)arg;
    for (int i = 0; i < 20; i++) {
        token_shared_read_lock(&selftest_ts);
        (void)selftest_counter;
        selftest_read_ops++;
        token_shared_read_unlock(&selftest_ts);
        lwkt_yield();
    }
    selftest_workers_done++;
    lwkt_thread_exit();
}

static void selftest_writer(void *arg) {
    (void)arg;
    for (int i = 0; i < 10; i++) {
        token_shared_write_lock(&selftest_ts);
        selftest_counter++;
        selftest_write_ops++;
        token_shared_write_unlock(&selftest_ts);
        lwkt_yield();
    }
    selftest_workers_done++;
    lwkt_thread_exit();
}

int token_shared_selftest(void) {
    struct token_shared local;
    token_shared_init(&local);

    if (!token_shared_read_trylock(&local)) {
        return -1;
    }
    if (!token_shared_read_trylock(&local)) {
        token_shared_read_unlock(&local);
        return -2;
    }
    token_shared_read_unlock(&local);
    token_shared_read_unlock(&local);

    if (token_shared_write_trylock(&local) == 0) {
        return -3;
    }
    token_shared_write_unlock(&local);

    if (token_shared_read_trylock(&local) == 0) {
        return -4;
    }
    if (token_shared_write_trylock(&local) != 0) {
        token_shared_read_unlock(&local);
        return -5;
    }
    token_shared_read_unlock(&local);
    return 0;
}

void token_shared_mp_selftest_start(void) {
    selftest_counter = 0;
    selftest_read_ops = 0;
    selftest_write_ops = 0;
    selftest_workers_done = 0;
    token_shared_init(&selftest_ts);

    if (!lwkt_create("ts_r1", selftest_reader, NULL, LWKT_PRIO_NORMAL) ||
        !lwkt_create("ts_r2", selftest_reader, NULL, LWKT_PRIO_NORMAL) ||
        !lwkt_create("ts_w", selftest_writer, NULL, LWKT_PRIO_NORMAL)) {
        console_writestring("token_shared MP selftest: create failed\n");
        return;
    }
    lwkt_create("ts_chk", selftest_checker, NULL, LWKT_PRIO_LOW);
}

static void selftest_checker(void *arg) {
    (void)arg;
    for (int spin = 0; spin < 500000; spin++) {
        if (selftest_workers_done >= 3) {
            break;
        }
        lwkt_yield();
    }

    if (selftest_workers_done >= 3 && selftest_counter == 10 &&
        selftest_write_ops == 10 && selftest_read_ops == 40) {
        console_writestring("token_shared MP selftest OK\n");
    } else {
        console_writestring("token_shared MP selftest FAILED\n");
    }
    lwkt_thread_exit();
}
