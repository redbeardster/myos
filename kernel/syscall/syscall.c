#include "syscall.h"

#include "console.h"
#include "cpu.h"
#include "exec.h"
#include "keyboard.h"
#include "kbdd.h"
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

    if (!lwkt_curthread()) {
        return -1;
    }

    int c = kbdd_request_char();
    if (c < 0) {
        return -1;
    }

    lwkt_preempt_check();
    return c;
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
    uint64_t ret;

    switch (num) {
        case SYS_EXIT: {
            struct uthread *u = uthread_current();
            if (u) {
                u->exit_code = (int)a1;
            }
            uthread_exit();
            __builtin_unreachable();
        }

        case SYS_WRITE:
            ret = (uint64_t)(int64_t)sys_write(a1, (const char *)(uintptr_t)a2, a3);
            break;

        case SYS_READ:
            ret = (uint64_t)(int64_t)sys_read(a1);
            break;

        case SYS_EXEC:
            ret = (uint64_t)(int64_t)sys_exec((const char *)(uintptr_t)a1);
            break;

        case SYS_YIELD:
            lwkt_yield();
            ret = 0;
            break;

        case SYS_MSG_SEND: {
            char tmp[MSG_MAX_PAYLOAD];
            uint64_t len = a3;
            if (len > MSG_MAX_PAYLOAD) {
                ret = (uint64_t)-2;
                break;
            }
            if (len > 0) {
                copy_from_user(tmp, (const char *)(uintptr_t)a2, len);
            }
            ret = (uint64_t)(int64_t)msg_send((uint32_t)a1, MSG_TYPE_DATA, tmp, (uint32_t)len);
            break;
        }

        case SYS_MSG_RECV: {
            struct msg m;
            int block = (int)a2;
            int rc = msg_receive(&m, block);
            if (rc != 0) {
                ret = (uint64_t)(int64_t)rc;
                break;
            }
            if (a1) {
                struct msg *out = (struct msg *)(uintptr_t)a1;
                *out = m;
            }
            ret = 0;
            break;
        }

        case SYS_MSGD_ID:
            ret = (uint64_t)msgd_thread_id();
            break;

        case SYS_MSG_PING: {
            static const char ping[] = "ping";
            if (msg_send_name("msgd", MSG_TYPE_PING, ping, 4) != 0) {
                ret = (uint64_t)-2;
                break;
            }
            struct msg m;
            if (msg_receive(&m, 1) != 0 || m.type != MSG_TYPE_PONG) {
                ret = (uint64_t)-3;
                break;
            }
            console_writestring("\n[pong from msgd] \"");
            for (uint32_t i = 0; i < m.size; i++) {
                char c = (char)m.data[i];
                if (c >= ' ' && c <= '~') {
                    console_putchar(c);
                } else {
                    console_putchar('.');
                }
            }
            console_writestring("\"\nMyOS> ");
            ret = 0;
            break;
        }

        case SYS_ALLOC:
            ret = (uint64_t)(uintptr_t)user_page_alloc();
            break;

        case SYS_FREE:
            user_free_page((void *)(uintptr_t)a1);
            ret = 0;
            break;

        case SYS_PS:
            proc_list();
            ret = 0;
            break;

        case SYS_THREADS:
            lwkt_list();
            ret = 0;
            break;

        case SYS_UTHREADS:
            uthread_list();
            ret = 0;
            break;

        case SYS_THREAD_CREATE: {
            struct proc *p = proc_current();
            if (!p) {
                ret = (uint64_t)-1;
                break;
            }
            uint32_t prio = LWKT_PRIO_NORMAL;
            if ((int64_t)a3 >= 0 && a3 < MAX_PRIORITY) {
                prio = (uint32_t)a3;
            }
            ret = (uint64_t)(int64_t)uthread_create_in_proc(p, a1, a2, prio);
            break;
        }

        case SYS_THREAD_JOIN: {
            int code = 0;
            int rc = uthread_join((uint32_t)a1, &code);
            if (rc != 0) {
                ret = (uint64_t)(int64_t)rc;
            } else {
                ret = (uint64_t)(int64_t)code;
            }
            break;
        }

        case SYS_MUTEX_LOCK:
            ret = (uint64_t)(int64_t)proc_mutex_lock((uint32_t)a1);
            break;

        case SYS_MUTEX_UNLOCK:
            ret = (uint64_t)(int64_t)proc_mutex_unlock((uint32_t)a1);
            break;

        case SYS_CPUS:
            cpu_list();
            ret = 0;
            break;

        case SYS_MSG_SEND_NAME: {
            char port[MSG_PORT_NAME_LEN];
            char tmp[MSG_MAX_PAYLOAD];
            if (copy_user_string(port, (const char *)(uintptr_t)a1, sizeof(port)) != 0) {
                ret = (uint64_t)-1;
                break;
            }
            uint64_t len = a3;
            if (len > MSG_MAX_PAYLOAD) {
                ret = (uint64_t)-2;
                break;
            }
            if (len > 0) {
                copy_from_user(tmp, (const char *)(uintptr_t)a2, len);
            }
            ret = (uint64_t)(int64_t)msg_send_name(port, MSG_TYPE_DATA, tmp, (uint32_t)len);
            break;
        }

        case SYS_PORT_LOOKUP: {
            char port[MSG_PORT_NAME_LEN];
            if (copy_user_string(port, (const char *)(uintptr_t)a1, sizeof(port)) != 0) {
                ret = (uint64_t)-1;
                break;
            }
            ret = (uint64_t)(int64_t)msgport_lookup(port);
            break;
        }

        case SYS_MSG_PORTS:
            msgport_list();
            ret = 0;
            break;

        default:
            ret = (uint64_t)-1;
            break;
    }

    lwkt_preempt_check();
    return ret;
}

void syscall_init(void) {
}
