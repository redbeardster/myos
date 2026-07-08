#include "kbdd.h"

#include "console.h"
#include "keyboard.h"
#include "lwkt.h"
#include "msgport.h"
#include "token.h"

#include <stdint.h>

#define KBDD_CHAR_RING  64
#define KBDD_MAX_WAITERS 8

static struct lwkt_thread *kbdd_thread;
static uint32_t kbdd_lwkt_id;

static struct {
    struct token lock;
    char ring[KBDD_CHAR_RING];
    int head;
    int tail;
    uint32_t waiters[KBDD_MAX_WAITERS];
    int waiter_count;
} kbdd;

static int char_ring_empty(void) {
    return kbdd.head == kbdd.tail;
}

static int char_ring_push(char c) {
    int next = (kbdd.tail + 1) % KBDD_CHAR_RING;
    if (next == kbdd.head) {
        return -1;
    }
    kbdd.ring[kbdd.tail] = c;
    kbdd.tail = next;
    return 0;
}

static int char_ring_pop(char *c) {
    if (char_ring_empty()) {
        return 0;
    }
    *c = kbdd.ring[kbdd.head];
    kbdd.head = (kbdd.head + 1) % KBDD_CHAR_RING;
    return 1;
}

static int waiter_add(uint32_t id) {
    for (int i = 0; i < kbdd.waiter_count; i++) {
        if (kbdd.waiters[i] == id) {
            return 0;
        }
    }
    if (kbdd.waiter_count >= KBDD_MAX_WAITERS) {
        return -1;
    }
    kbdd.waiters[kbdd.waiter_count++] = id;
    return 0;
}

static void waiter_remove_at(int idx) {
    for (int i = idx + 1; i < kbdd.waiter_count; i++) {
        kbdd.waiters[i - 1] = kbdd.waiters[i];
    }
    kbdd.waiter_count--;
}

static void send_char_to(uint32_t client_id, char c) {
    uint8_t b = (uint8_t)c;
    msg_send(client_id, MSG_TYPE_KBD_CHAR, &b, 1);
}

static void kbdd_drain_scancodes(void) {
    uint8_t sc;
    while (keyboard_pop_scancode(&sc) || keyboard_poll_scancode(&sc)) {
        char c = keyboard_translate_scancode(sc);
        if (c == 0) {
            continue;
        }
        token_lock(&kbdd.lock);
        char_ring_push(c);
        token_unlock(&kbdd.lock);
    }
}

static void kbdd_serve_waiters(void) {
    for (;;) {
        uint32_t client = 0;
        char c = 0;
        int found = 0;

        token_lock(&kbdd.lock);
        if (kbdd.waiter_count > 0 && char_ring_pop(&c)) {
            client = kbdd.waiters[0];
            waiter_remove_at(0);
            found = 1;
        }
        token_unlock(&kbdd.lock);

        if (!found) {
            break;
        }
        send_char_to(client, c);
    }
}

static void kbdd_handle_wait(uint32_t client_id) {
    if (client_id == 0) {
        return;
    }

    char c = 0;
    int deliver = 0;

    token_lock(&kbdd.lock);
    if (char_ring_pop(&c)) {
        deliver = 1;
    } else {
        waiter_add(client_id);
    }
    token_unlock(&kbdd.lock);

    if (deliver) {
        send_char_to(client_id, c);
    }
}

static int kbdd_has_work(void) {
    if (keyboard_scancode_pending()) {
        return 1;
    }
    token_lock(&kbdd.lock);
    /* Waiters alone are not work — block until IRQ delivers a scancode. */
    int work = kbdd.waiter_count > 0 && !char_ring_empty();
    token_unlock(&kbdd.lock);
    return work;
}

void kbdd_irq_notify(void) {
    if (kbdd_thread && kbdd_thread->state == THREAD_BLOCKED) {
        lwkt_unblock(kbdd_thread);
    }
}

void kbdd_worker(void *arg) {
    (void)arg;

    for (;;) {
        kbdd_drain_scancodes();
        kbdd_serve_waiters();

        struct msg m;
        int rc = msg_try_receive(&m);
        if (rc == 0) {
            if (m.type == MSG_TYPE_KBD_WAIT) {
                kbdd_handle_wait(m.from);
            }
            continue;
        }

        if (kbdd_has_work()) {
            continue;
        }

        lwkt_block();
    }
}

int kbdd_start(void) {
    kbdd.head = 0;
    kbdd.tail = 0;
    kbdd.waiter_count = 0;
    token_init(&kbdd.lock);

    struct lwkt_thread *t = lwkt_create("kbdd", kbdd_worker, NULL, LWKT_PRIO_NORMAL);
    if (!t) {
        return -1;
    }
    kbdd_thread = t;
    kbdd_lwkt_id = t->id;

    if (msgport_register("kbdd", t) != 0) {
        console_writestring("msgport_register(kbdd) failed\n");
    }
    return 0;
}

uint32_t kbdd_thread_id(void) {
    return kbdd_lwkt_id;
}

int kbdd_request_char(void) {
    struct lwkt_thread *self = lwkt_curthread();
    if (!self) {
        return -1;
    }

    /*
     * From int 0x80 the runner LWKT stays RUNNING across hlt; kbdd never
     * gets scheduled to handle MSG_KBD_WAIT. Poll the IRQ scancode ring
     * directly instead of blocking on msgport.
     */
    if (lwkt_in_usersyscall()) {
        for (;;) {
            kbdd_drain_scancodes();

            char c = 0;
            token_lock(&kbdd.lock);
            int ok = char_ring_pop(&c);
            token_unlock(&kbdd.lock);
            if (ok) {
                return (int)(unsigned char)c;
            }

            lwkt_syscall_wait_edge();
        }
    }

    if (msg_send_name("kbdd", MSG_TYPE_KBD_WAIT, NULL, 0) != 0) {
        return -1;
    }

    struct msg reply;
    if (msg_receive(&reply, 1) != 0) {
        return -1;
    }
    if (reply.type != MSG_TYPE_KBD_CHAR || reply.size < 1) {
        return -1;
    }
    return (int)(unsigned char)reply.data[0];
}
