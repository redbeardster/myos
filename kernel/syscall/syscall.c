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

static uint64_t ping_non_pong_skips;

static void syscall_stash_user_callee(struct uthread *u, uint64_t rbx, uint64_t rbp,
                                     uint64_t r12, uint64_t r13, uint64_t r14,
                                     uint64_t r15) {
    if (!uthread_ptr_valid(u)) {
        return;
    }

    u->user_rbx = rbx;
    u->user_rbp = rbp;
    u->user_r12 = r12;
    u->user_r13 = r13;
    u->user_r14 = r14;
    u->user_r15 = r15;
    u->user_regs_valid = 1;
}

static int copy_from_user_safe(char *dst, const char *src, uint64_t len) {
    uint64_t s = (uint64_t)(uintptr_t)src;
    if (s < MYOS_USER_BASE || s + len > MYOS_USER_STACK_TOP || s + len < s) {
        return -1;
    }
    for (uint64_t i = 0; i < len; i++) {
        dst[i] = src[i];
    }
    return 0;
}

static int copy_user_string(char *dst, const char *src, uint64_t max) {
    if (!dst || !src || max == 0) {
        return -1;
    }

    uint64_t s = (uint64_t)(uintptr_t)src;
    if (s < MYOS_USER_BASE || s + max > MYOS_USER_STACK_TOP || s + max < s) {
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
    if (copy_from_user_safe(tmp, buf, len) != 0) {
        return -1;
    }
    console_write_n(tmp, len);
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

static int syscall_msg_receive(struct msg *out, int block) {
    if (!block || !lwkt_in_usersyscall()) {
        return msg_receive(out, block);
    }

    int rc = msg_receive(out, 0);
    if (rc == 1) {
        lwkt_syscall_resched(MYOS_ERR_AGAIN);
        return MYOS_ERR_AGAIN;
    }
    return rc;
}

static int syscall_wait_pong(struct msg *out) {
    for (;;) {
        int rc = msg_receive(out, 0);
        if (rc == 1) {
            if (lwkt_in_usersyscall()) {
                lwkt_syscall_resched(MYOS_ERR_AGAIN);
                return MYOS_ERR_AGAIN;
            }
            continue;
        }
        if (rc != 0) {
            return rc;
        }
        if (out->type == MSG_TYPE_PONG) {
            return 0;
        }
        ping_non_pong_skips++;
        if ((ping_non_pong_skips & 0x3F) == 0) {
            console_writestring("\n[ping] skipped non-PONG messages=");
            console_write_dec(ping_non_pong_skips);
            console_writestring("\nMyOS> ");
        }
        if (lwkt_in_usersyscall()) {
            lwkt_syscall_resched(MYOS_ERR_AGAIN);
            return MYOS_ERR_AGAIN;
        }
    }
}

uint64_t syscall_dispatch(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3,
                          uint64_t user_rip, uint64_t user_rsp,
                          uint64_t user_rbx, uint64_t user_rbp,
                          uint64_t user_r12, uint64_t user_r13,
                          uint64_t user_r14, uint64_t user_r15) {
    struct lwkt_thread *cur = lwkt_curthread();
    if (cur) {
        cur->in_syscall = 1;
        if (cur->user_proc && cur->user_proc->current_uthread) {
            struct uthread *u = cur->user_proc->current_uthread;
            if (uthread_ptr_valid(u)) {
                syscall_stash_user_callee(u, user_rbx, user_rbp, user_r12, user_r13,
                                          user_r14, user_r15);
                if (user_rip >= MYOS_USER_BASE && user_rip < MYOS_USER_STACK_TOP) {
                    u->user_rip = user_rip;
                }
                if (user_rsp >= MYOS_USER_STACK_BASE && user_rsp < MYOS_USER_STACK_TOP) {
                    u->user_rsp = user_rsp;
                }
                u->user_rdi = a1;
            }
        }
    }

    uint64_t ret = 0;

    switch (num) {
        case SYS_EXIT: {
            struct uthread *u = uthread_current();
            if (u) {
                u->exit_code = (int)a1;
            }
            uthread_exit();
            break;
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
            uthread_yield();
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
                if (copy_from_user_safe(tmp, (const char *)(uintptr_t)a2, len) != 0) {
                    ret = (uint64_t)-1;
                    break;
                }
            }
            ret = (uint64_t)(int64_t)msg_send((uint32_t)a1, MSG_TYPE_DATA, tmp, (uint32_t)len);
            break;
        }

        case SYS_MSG_RECV: {
            struct msg m;
            int block = (int)a2;
            int rc = syscall_msg_receive(&m, block);
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
            int silent = (a1 & MYOS_MSG_PING_SILENT) != 0;
            if (a1 & MYOS_MSG_PING_SEND) {
                if (msg_send_name("msgd", MSG_TYPE_PING, ping, 4) != 0) {
                    ret = (uint64_t)-2;
                    break;
                }
            }
            struct msg m;
            int rc = syscall_wait_pong(&m);
            if (rc != 0) {
                ret = (uint64_t)(int64_t)rc;
                break;
            }
            if (!silent) {
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
            }
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
                if (copy_from_user_safe(tmp, (const char *)(uintptr_t)a2, len) != 0) {
                    ret = (uint64_t)-1;
                    break;
                }
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

        case SYS_IPC_BUMP_MODE:
            ret = (uint64_t)(int64_t)lwkt_ipc_bump_mode((int)a1);
            break;

        case SYS_CAP_CREATE_PORT:
            ret = (uint64_t)(int64_t)proc_cap_create_port();
            break;

        case SYS_CAP_SEND: {
            char tmp[MSG_MAX_PAYLOAD];
            uint64_t len = a3;
            if (len > MSG_MAX_PAYLOAD) {
                ret = (uint64_t)-2;
                break;
            }
            if (len > 0) {
                if (copy_from_user_safe(tmp, (const char *)(uintptr_t)a2, len) != 0) {
                    ret = (uint64_t)-1;
                    break;
                }
            }
            ret = (uint64_t)(int64_t)proc_cap_send((uint32_t)a1, MSG_TYPE_DATA, tmp,
                                                   (uint32_t)len);
            break;
        }

        case SYS_CAP_RECV: {
            struct msg m;
            int block = (int)a3;
            int rc = proc_cap_recv((uint32_t)a1, &m, block);
            if (rc != 0) {
                ret = (uint64_t)(int64_t)rc;
                break;
            }
            if (a2) {
                struct msg *out = (struct msg *)(uintptr_t)a2;
                *out = m;
            }
            ret = 0;
            break;
        }

        case SYS_CAP_GRANT:
            ret = (uint64_t)(int64_t)proc_cap_grant((uint32_t)a1, (uint32_t)a2, (uint32_t)a3);
            break;

        case SYS_CAP_CLOSE:
            ret = (uint64_t)(int64_t)proc_cap_close((uint32_t)a1);
            break;

        case SYS_GETPID: {
            struct proc *p = proc_current();
            ret = p ? (uint64_t)p->pid : 0;
            break;
        }

        default:
            ret = (uint64_t)-1;
            break;
    }

    lwkt_preempt_check();
    return ret;
}

uint64_t syscall_post_dispatch(uint64_t ret) {
    struct lwkt_thread *cur = lwkt_curthread();
    if (cur) {
        if (cur->pending_kill) {
            cur->pending_kill = 0;
            cur->in_syscall = 0;
            runner_longjmp(&cur->runner_jmp, 1);
        }
        if (cur->runner_reswitch) {
            cur->runner_reswitch = 0;
            cur->in_syscall = 0;
            runner_longjmp(&cur->runner_jmp, 1);
        }
        cur->in_syscall = 0;
    }
    return ret;
}

void syscall_init(void) {
}
