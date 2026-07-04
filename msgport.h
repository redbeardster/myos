#ifndef MSGPORT_H
#define MSGPORT_H

#include <stdint.h>

struct lwkt_thread;

#define MSG_MAX_PAYLOAD 64
#define MSG_QUEUE_DEPTH 8

#define MSG_TYPE_DATA 0
#define MSG_TYPE_PING 1
#define MSG_TYPE_PONG 2
#define MSG_TYPE_WAKEUP 0xFE

struct msg {
    uint32_t from;
    uint32_t type;
    uint32_t size;
    uint8_t data[MSG_MAX_PAYLOAD];
};

struct ping_ctx {
    uint32_t peer;
    int in_use;
};

struct ping_ctx *msg_ping_ctx_alloc(uint32_t peer);
void msg_ping_ctx_free(struct ping_ctx *ctx);
int msg_pingdemo_pair(uint32_t *id_a, uint32_t *id_b);

void msgport_init(void);
void msgport_clear_slot(uint8_t slot);

int msg_send(uint32_t to_id, uint32_t type, const void *data, uint32_t size);
int msg_receive(struct msg *out, int block);
int msg_try_receive(struct msg *out);

int msgport_wakeup(struct lwkt_thread *t);

void msg_echo_worker(void *arg);
void msg_ping_worker(void *arg);

#endif
