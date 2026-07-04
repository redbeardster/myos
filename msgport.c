#include "msgport.h"

#include "console.h"
#include "lwkt.h"

#include <stddef.h>
#include <stdint.h>

#define MAX_MAILBOXES (MAX_THREADS + 1)

struct mailbox {
    struct msg queue[MSG_QUEUE_DEPTH];
    int head;
    int tail;
    int count;
};

static struct mailbox mailboxes[MAX_MAILBOXES];

#define MAX_PING_CTX 8
static struct ping_ctx ping_ctx_pool[MAX_PING_CTX];

struct ping_ctx *msg_ping_ctx_alloc(uint32_t peer) {
    for (int i = 0; i < MAX_PING_CTX; i++) {
        if (!ping_ctx_pool[i].in_use) {
            ping_ctx_pool[i].peer = peer;
            ping_ctx_pool[i].in_use = 1;
            return &ping_ctx_pool[i];
        }
    }
    return NULL;
}

void msg_ping_ctx_free(struct ping_ctx *ctx) {
    if (ctx) {
        ctx->in_use = 0;
    }
}

static void memcpy_local(void *dst, const void *src, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) {
        *d++ = *s++;
    }
}

static void irq_disable(void) {
    __asm__ volatile("cli");
}

static void irq_enable(void) {
    __asm__ volatile("sti");
}

static struct mailbox *mbox_for_thread(struct lwkt_thread *t) {
    if (!t || t->mbox_slot >= MAX_MAILBOXES) {
        return NULL;
    }
    return &mailboxes[t->mbox_slot];
}

static int mbox_push(struct mailbox *mb, const struct msg *m) {
    if (!mb || mb->count >= MSG_QUEUE_DEPTH) {
        return -1;
    }
    mb->queue[mb->tail] = *m;
    mb->tail = (mb->tail + 1) % MSG_QUEUE_DEPTH;
    mb->count++;
    return 0;
}

static int mbox_pop(struct mailbox *mb, struct msg *out) {
    if (!mb || mb->count == 0) {
        return -1;
    }
    *out = mb->queue[mb->head];
    mb->head = (mb->head + 1) % MSG_QUEUE_DEPTH;
    mb->count--;
    return 0;
}

void msgport_init(void) {
    for (int i = 0; i < MAX_MAILBOXES; i++) {
        mailboxes[i].head = 0;
        mailboxes[i].tail = 0;
        mailboxes[i].count = 0;
    }
}

void msgport_clear_slot(uint8_t slot) {
    if (slot >= MAX_MAILBOXES) {
        return;
    }
    irq_disable();
    mailboxes[slot].head = 0;
    mailboxes[slot].tail = 0;
    mailboxes[slot].count = 0;
    irq_enable();
}

int msg_send(uint32_t to_id, uint32_t type, const void *data, uint32_t size) {
    struct lwkt_thread *to = lwkt_find(to_id);
    if (!to || (to_id != 0 && to->id == 0)) {
        return -1;
    }
    if (size > MSG_MAX_PAYLOAD) {
        return -2;
    }

    struct lwkt_thread *from = lwkt_curthread();
    struct msg m;
    m.from = from ? from->id : 0;
    m.type = type;
    m.size = size;
    if (data && size > 0) {
        memcpy_local(m.data, data, size);
    }

    struct mailbox *mb = mbox_for_thread(to);
    if (!mb) {
        return -1;
    }

    irq_disable();
    if (mbox_push(mb, &m) != 0) {
        irq_enable();
        return -3;
    }

    int wake = (to->state == THREAD_BLOCKED);
    irq_enable();

    if (wake) {
        lwkt_unblock(to);
    }
    return 0;
}

int msg_try_receive(struct msg *out) {
    return msg_receive(out, 0);
}

int msg_receive(struct msg *out, int block) {
    if (!out) {
        return -1;
    }

    struct lwkt_thread *self = lwkt_curthread();
    if (!self) {
        return -1;
    }

    struct mailbox *mb = mbox_for_thread(self);
    if (!mb) {
        return -1;
    }

    for (;;) {
        if (self->state == THREAD_TERMINATED) {
            return -1;
        }

        irq_disable();
        if (mbox_pop(mb, out) == 0) {
            irq_enable();
            if (out->type == MSG_TYPE_WAKEUP || self->state == THREAD_TERMINATED) {
                return -1;
            }
            return 0;
        }
        irq_enable();

        if (!block) {
            return 1;
        }

        lwkt_block();
    }
}

static void print_msg_line(const struct msg *m) {
    console_writestring("\n[msg ");
    console_write_dec(lwkt_curthread()->id);
    console_writestring("] from ");
    console_write_dec(m->from);
    console_writestring(" type=");
    console_write_dec(m->type);
    console_writestring(" data=\"");

    for (uint32_t i = 0; i < m->size; i++) {
        char c = (char)m->data[i];
        if (c >= ' ' && c <= '~') {
            console_putchar(c);
        } else {
            console_putchar('.');
        }
    }
    console_writestring("\"\nMyOS> ");
}

int msgport_wakeup(struct lwkt_thread *t) {
    struct mailbox *mb = mbox_for_thread(t);
    if (!mb) {
        return -1;
    }

    struct msg wake = {0};
    wake.type = MSG_TYPE_WAKEUP;

    irq_disable();
    int rc = mbox_push(mb, &wake);
    int blocked = (t->state == THREAD_BLOCKED);
    irq_enable();

    if (rc != 0) {
        return -1;
    }
    if (blocked) {
        lwkt_unblock(t);
    }
    return 0;
}

void msg_echo_worker(void *arg) {
    (void)arg;
    struct msg m;
    for (;;) {
        if (msg_receive(&m, 1) != 0) {
            break;
        }
        if (m.type == MSG_TYPE_DATA) {
            print_msg_line(&m);
        }
    }
}

void msg_ping_worker(void *arg) {
    struct ping_ctx *ctx = (struct ping_ctx *)arg;
    struct msg m;

    if (!ctx) {
        return;
    }

    for (;;) {
        if (msg_receive(&m, 1) != 0) {
            break;
        }

        if (m.type == MSG_TYPE_PING) {
            msg_send(ctx->peer, MSG_TYPE_PONG, m.data, m.size);
        } else if (m.type == MSG_TYPE_PONG) {
            print_msg_line(&m);
        }
    }
}

int msg_pingdemo_pair(uint32_t *id_a, uint32_t *id_b) {
    struct ping_ctx *ctx1 = msg_ping_ctx_alloc(0);
    struct ping_ctx *ctx2 = msg_ping_ctx_alloc(0);
    if (!ctx1 || !ctx2) {
        if (ctx1) {
            msg_ping_ctx_free(ctx1);
        }
        if (ctx2) {
            msg_ping_ctx_free(ctx2);
        }
        return -1;
    }

    struct lwkt_thread *t1 = lwkt_create("pong", msg_ping_worker, ctx1, LWKT_PRIO_NORMAL);
    struct lwkt_thread *t2 = lwkt_create("p2", msg_ping_worker, ctx2, LWKT_PRIO_NORMAL);
    if (!t1 || !t2) {
        msg_ping_ctx_free(ctx1);
        msg_ping_ctx_free(ctx2);
        return -1;
    }

    ctx1->peer = t2->id;
    ctx2->peer = t1->id;

    if (id_a) {
        *id_a = t1->id;
    }
    if (id_b) {
        *id_b = t2->id;
    }
    return 0;
}
