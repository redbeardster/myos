#include "msgport.h"

#include "console.h"
#include "lwkt.h"
#include "spinlock.h"
#include "token.h"
#include "uthread.h"

#include <stddef.h>
#include <stdint.h>

#define MAX_MAILBOXES (MAX_THREADS + 1)

struct mailbox {
    struct token lock;
    struct lwkt_thread *read_waiters;
    struct msg queue[MSG_QUEUE_DEPTH];
    int head;
    int tail;
    int count;
};

static struct mailbox mailboxes[MAX_MAILBOXES];

struct named_port {
    char name[MSG_PORT_NAME_LEN];
    uint32_t lwkt_id;
    int in_use;
};

static struct named_port named_ports[MSG_PORT_MAX];
static spinlock_t named_ports_lock;

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

static struct mailbox *mbox_for_thread(struct lwkt_thread *t) {
    if (!t || t->mbox_slot >= MAX_MAILBOXES) {
        return NULL;
    }
    return &mailboxes[t->mbox_slot];
}

static void read_wait_enqueue(struct mailbox *mb, struct lwkt_thread *self) {
    self->mbox_wait_next = NULL;
    if (!mb->read_waiters) {
        mb->read_waiters = self;
        return;
    }
    struct lwkt_thread *tail = mb->read_waiters;
    while (tail->mbox_wait_next) {
        tail = tail->mbox_wait_next;
    }
    tail->mbox_wait_next = self;
}

static struct lwkt_thread *read_wait_dequeue(struct mailbox *mb) {
    struct lwkt_thread *w = mb->read_waiters;
    if (!w) {
        return NULL;
    }
    mb->read_waiters = w->mbox_wait_next;
    w->mbox_wait_next = NULL;
    return w;
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
    spin_init(&named_ports_lock);
    for (int i = 0; i < MSG_PORT_MAX; i++) {
        named_ports[i].in_use = 0;
        named_ports[i].name[0] = '\0';
        named_ports[i].lwkt_id = 0;
    }
    for (int i = 0; i < MAX_MAILBOXES; i++) {
        token_init(&mailboxes[i].lock);
        mailboxes[i].read_waiters = NULL;
        mailboxes[i].head = 0;
        mailboxes[i].tail = 0;
        mailboxes[i].count = 0;
    }
}

void msgport_clear_slot(uint8_t slot) {
    if (slot >= MAX_MAILBOXES) {
        return;
    }

    struct mailbox *mb = &mailboxes[slot];
    token_lock(&mb->lock);
    for (struct lwkt_thread *w = mb->read_waiters; w; w = w->mbox_wait_next) {
        w->mbox_wait_next = NULL;
    }
    mb->read_waiters = NULL;
    mb->head = 0;
    mb->tail = 0;
    mb->count = 0;
    token_unlock(&mb->lock);
}

static void strcpy_port_name(char *dst, const char *src) {
    int i = 0;
    while (src[i] && i < MSG_PORT_NAME_LEN - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

int msgport_register(const char *name, struct lwkt_thread *owner) {
    if (!name || !name[0] || !owner || owner->id == 0) {
        return -1;
    }

    uint64_t irqf;
    spin_lock_irqsave(&named_ports_lock, &irqf);

    for (int i = 0; i < MSG_PORT_MAX; i++) {
        if (named_ports[i].in_use &&
            named_ports[i].lwkt_id == owner->id) {
            strcpy_port_name(named_ports[i].name, name);
            spin_unlock_irqrestore(&named_ports_lock, irqf);
            return 0;
        }
    }

    for (int i = 0; i < MSG_PORT_MAX; i++) {
        if (!named_ports[i].in_use) {
            strcpy_port_name(named_ports[i].name, name);
            named_ports[i].lwkt_id = owner->id;
            named_ports[i].in_use = 1;
            spin_unlock_irqrestore(&named_ports_lock, irqf);
            return 0;
        }
    }

    spin_unlock_irqrestore(&named_ports_lock, irqf);
    return -2;
}

void msgport_unregister_id(uint32_t lwkt_id) {
    if (lwkt_id == 0) {
        return;
    }

    uint64_t irqf;
    spin_lock_irqsave(&named_ports_lock, &irqf);
    for (int i = 0; i < MSG_PORT_MAX; i++) {
        if (named_ports[i].in_use && named_ports[i].lwkt_id == lwkt_id) {
            named_ports[i].in_use = 0;
            named_ports[i].name[0] = '\0';
            named_ports[i].lwkt_id = 0;
        }
    }
    spin_unlock_irqrestore(&named_ports_lock, irqf);
}

int msgport_lookup(const char *name) {
    if (!name || !name[0]) {
        return -1;
    }

    uint64_t irqf;
    spin_lock_irqsave(&named_ports_lock, &irqf);
    for (int i = 0; i < MSG_PORT_MAX; i++) {
        if (!named_ports[i].in_use) {
            continue;
        }
        const char *a = name;
        const char *b = named_ports[i].name;
        int match = 1;
        for (int j = 0; j < MSG_PORT_NAME_LEN; j++) {
            if (*a != *b) {
                match = 0;
                break;
            }
            if (*a == '\0') {
                break;
            }
            a++;
            b++;
        }
        if (match) {
            int id = (int)named_ports[i].lwkt_id;
            spin_unlock_irqrestore(&named_ports_lock, irqf);
            return id;
        }
    }
    spin_unlock_irqrestore(&named_ports_lock, irqf);
    return -1;
}

int msg_send_name(const char *name, uint32_t type, const void *data, uint32_t size) {
    int id = msgport_lookup(name);
    if (id < 0) {
        return -1;
    }
    return msg_send((uint32_t)id, type, data, size);
}

void msgport_list(void) {
    console_writestring("\nNamed msgports:\n");
    console_writestring("  Name            LWKT id\n");
    console_writestring("  --------------  -------\n");

    uint64_t irqf;
    spin_lock_irqsave(&named_ports_lock, &irqf);
    int count = 0;
    for (int i = 0; i < MSG_PORT_MAX; i++) {
        if (!named_ports[i].in_use) {
            continue;
        }
        count++;
        console_writestring("  ");
        console_writestring(named_ports[i].name);
        console_writestring("  ");
        console_write_dec(named_ports[i].lwkt_id);
        console_putchar('\n');
    }
    spin_unlock_irqrestore(&named_ports_lock, irqf);

    console_writestring("\nTotal: ");
    console_write_dec((uint64_t)count);
    console_writestring(" port(s)\n");
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

    struct lwkt_thread *wake = NULL;
    token_lock(&mb->lock);
    if (mbox_push(mb, &m) != 0) {
        token_unlock(&mb->lock);
        return -3;
    }
    wake = read_wait_dequeue(mb);
    token_unlock(&mb->lock);

    if (wake) {
        lwkt_unblock(wake);
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

        if (block) {
            token_lock(&mb->lock);
        } else if (!token_trylock(&mb->lock)) {
            return 1;
        }

        if (mbox_pop(mb, out) == 0) {
            token_unlock(&mb->lock);
            if (out->type == MSG_TYPE_WAKEUP || self->state == THREAD_TERMINATED) {
                return -1;
            }
            return 0;
        }

        if (!block) {
            token_unlock(&mb->lock);
            return 1;
        }

        int already = 0;
        for (struct lwkt_thread *w = mb->read_waiters; w; w = w->mbox_wait_next) {
            if (w == self) {
                already = 1;
                break;
            }
        }
        if (!already) {
            read_wait_enqueue(mb, self);
        }

        if (lwkt_in_usersyscall()) {
            token_unlock(&mb->lock);
            lwkt_syscall_wait_edge();
        } else {
            self->state = THREAD_BLOCKED;
            token_unlock(&mb->lock);
            lwkt_switch();
        }
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

    struct lwkt_thread *reader = NULL;
    token_lock(&mb->lock);
    int rc = mbox_push(mb, &wake);
    if (rc == 0) {
        reader = read_wait_dequeue(mb);
    }
    token_unlock(&mb->lock);

    if (rc != 0) {
        return -1;
    }
    if (reader) {
        lwkt_unblock(reader);
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

static uint32_t msgd_lwkt_id;

void msgd_worker(void *arg) {
    (void)arg;
    struct msg m;

    for (;;) {
        if (msg_receive(&m, 1) != 0) {
            break;
        }

        if (m.type == MSG_TYPE_DATA) {
            print_msg_line(&m);
        } else if (m.type == MSG_TYPE_PING) {
            msg_send(m.from, MSG_TYPE_PONG, m.data, m.size);
        }
    }
}

int msgd_start(void) {
    struct uthread *u = uthread_spawn("msgd", msgd_worker, NULL, LWKT_PRIO_NORMAL);
    if (!u || !u->lwkt) {
        return -1;
    }
    msgd_lwkt_id = u->uthread_id;
    if (msgport_register("msgd", u->lwkt) != 0) {
        console_writestring("msgport_register(msgd) failed\n");
    }
    return 0;
}

uint32_t msgd_thread_id(void) {
    return msgd_lwkt_id;
}
