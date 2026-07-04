#include "syscall.h"

#include "console.h"
#include "exec.h"
#include "keyboard.h"
#include "lwkt.h"
#include "memory.h"
#include "msgport.h"
#include "proc.h"
#include "user.h"
#include "uthread.h"

static void copy_from_user(char *dst, const char *src, uint64_t len) {
    for (uint64_t i = 0; i < len; i++) {
        dst[i] = src[i];
    }
}

static int copy_user_string(char *dst, const char *src, uint64_t max) {
    if (!dst || !src || max == 0) {
        return -1;
    }

    uint64_t i = 0;
    for (; i + 1 < max; i++) {
        char c = src[i];
        dst[i] = c;
        if (c == '\0') {
            return 0;
        }
    }
    dst[max - 1] = '\0';
    return -2;
}

static int sys_write(uint64_t fd, const char *buf, uint64_t len) {
    if (fd != 1 && fd != 2) {
        return -1;
    }
    if (!buf || len == 0) {
        return 0;
    }
    if (len > 256) {
        len = 256;
    }

    char tmp[256];
    copy_from_user(tmp, buf, len);
    for (uint64_t i = 0; i < len; i++) {
        console_putchar(tmp[i]);
    }
    return (int)len;
}

static int sys_read(uint64_t fd) {
    if (fd != 0) {
        return -1;
    }

    struct lwkt_thread *self = lwkt_curthread();

    for (;;) {
        char c = keyboard_poll_char();
        if (c != 0) {
            keyboard_clear_reader(self);
            return (int)(unsigned char)c;
        }

        keyboard_set_reader(self);
        lwkt_block();
    }
}

static int sys_exec(const char *name) {
    char path[64];
    if (copy_user_string(path, name, sizeof(path)) != 0) {
        return -2;
    }
    if (path[0] == '\0') {
        return -2;
    }
    return exec_spawn_module(path, 0);
}

uint64_t syscall_dispatch(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3) {
    switch (num) {
        case SYS_EXIT:
            uthread_exit();
            __builtin_unreachable();

        case SYS_WRITE:
            return (uint64_t)(int64_t)sys_write(a1, (const char *)(uintptr_t)a2, a3);

        case SYS_READ:
            return (uint64_t)(int64_t)sys_read(a1);

        case SYS_EXEC:
            return (uint64_t)(int64_t)sys_exec((const char *)(uintptr_t)a1);

        case SYS_YIELD:
            lwkt_yield();
            return 0;

        case SYS_MSG_SEND: {
            char tmp[MSG_MAX_PAYLOAD];
            uint64_t len = a3;
            if (len > MSG_MAX_PAYLOAD) {
                return (uint64_t)-2;
            }
            if (len > 0) {
                copy_from_user(tmp, (const char *)(uintptr_t)a2, len);
            }
            return (uint64_t)(int64_t)msg_send((uint32_t)a1, MSG_TYPE_DATA, tmp, (uint32_t)len);
        }

        case SYS_MSG_RECV: {
            struct msg m;
            int block = (int)a2;
            int rc = msg_receive(&m, block);
            if (rc != 0) {
                return (uint64_t)(int64_t)rc;
            }
            if (a1) {
                struct msg *out = (struct msg *)(uintptr_t)a1;
                *out = m;
            }
            return 0;
        }

        case SYS_ALLOC:
            return (uint64_t)(uintptr_t)user_page_alloc();

        case SYS_FREE:
            user_free_page((void *)(uintptr_t)a1);
            return 0;

        case SYS_PS:
            proc_list();
            return 0;

        case SYS_THREADS:
            lwkt_list();
            return 0;

        default:
            return (uint64_t)-1;
    }
}

void syscall_init(void) {
}
