#include "myos.h"

static int str_eq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == *b;
}

static int str_starts(const char *s, const char *prefix) {
    while (*prefix) {
        if (*s++ != *prefix++) {
            return 0;
        }
    }
    return 1;
}

static void skip_spaces(const char **s) {
    while (**s == ' ') {
        (*s)++;
    }
}

static unsigned long str_len(const char *s) {
    unsigned long n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

static long parse_u32(const char *s, unsigned long *out) {
    unsigned long v = 0;
    if (!s || *s == '\0') {
        return -1;
    }
    while (*s) {
        if (*s < '0' || *s > '9') {
            return -1;
        }
        v = v * 10 + (unsigned long)(*s - '0');
        s++;
    }
    *out = v;
    return 0;
}

static void write_str(const char *s) {
    myos_write(1, s, str_len(s));
}

static void write_dec(unsigned long v) {
    char num[24];
    int n = 0;
    if (v == 0) {
        num[n++] = '0';
    } else {
        char tmp[24];
        int t = 0;
        while (v > 0) {
            tmp[t++] = (char)('0' + (v % 10));
            v /= 10;
        }
        while (t > 0) {
            num[n++] = tmp[--t];
        }
    }
    num[n] = '\0';
    write_str(num);
}

static void write_prompt(void) {
    write_str("\nMyOS> ");
}

static void write_rc(long rc) {
    if (rc < 0) {
        write_str("-");
        write_dec((unsigned long)(0 - rc));
    } else {
        write_dec((unsigned long)rc);
    }
}

static int read_line(char *buf, int max) {
    int i = 0;
    while (i < max - 1) {
        long c = myos_read_char();
        if (c < 0) {
            return -1;
        }

        if (c == '\b') {
            if (i > 0) {
                i--;
                write_str("\b \b");
            }
            continue;
        }

        if (c == '\n') {
            buf[i] = '\0';
            myos_write(1, "\n", 1);
            return i;
        }

        if (c >= ' ' && c <= '~') {
            buf[i++] = (char)c;
            char ch[1] = {(char)c};
            myos_write(1, ch, 1);
        }
    }
    buf[i] = '\0';
    return i;
}

static void cmd_help(void) {
    write_str("\nUserland shell commands:\n");
    write_str("  help            - this message\n");
    write_str("  about           - system info\n");
    write_str("  echo <text>     - print text\n");
    write_str("  exec [name]     - run ELF (default hello.elf)\n");
    write_str("  spin            - run long-lived spin.elf workload\n");
    write_str("  ps              - list processes (CR3, uthreads)\n");
    write_str("  uthreads        - list uthreads (slot, proc, lwkt)\n");
    write_str("  cpus            - SMP CPU status (per-CPU scheduler)\n");
    write_str("  smpbalance      - per-CPU runner/LWKT distribution\n");
    write_str("  smpbench        - exec spin.elf + SMP snapshot\n");
    write_str("  kill <pid>      - kill process by pid (except shell)\n");
    write_str("  killall <name>  - kill all child processes by name\n");
    write_str("  threads         - list LWKT scheduler threads\n");
    write_str("  ports           - list named msgports\n");
    write_str("  msg [port] text - send to msgport (default msgd)\n");
    write_str("  ping            - msgport ping/pong with msgd\n");
    write_str("  ipcmode [on/off]- toggle IPC receiver bump\n");
    write_str("  pingbench [N]   - run N ping calls\n");
    write_str("  pingbench_ab [N]- compare ping with bump off/on\n");
    write_str("  capsmoke        - capability self-send/recv test\n");
    write_str("  capnew          - create local capability slot\n");
    write_str("  capsend S text  - send text via cap slot S\n");
    write_str("  caprecv S [b]   - recv via cap slot S (b=1 block)\n");
    write_str("  capgrant S P R  - grant slot S to pid P with rights R\n");
    write_str("  capclose S      - close/free cap slot S\n");
    write_str("  capdiag         - run capability diagnostics\n");
    write_str("  capdiag stress N- run diagnostics N times\n");
    write_str("  capdiag grantstress N - stress grant path\n");
    write_str("  yield           - syscall yield test\n");
}

static void cmd_about(void) {
    write_str("\nMyOS userland shell (ring 3)\n");
    write_str("Kernel: LWKT + per-process CR3 + MyOS syscalls\n");
}

static void run_command(char *line) {
    if (line[0] == '\0') {
        return;
    }

    if (str_eq(line, "help")) {
        cmd_help();
        return;
    }

    if (str_eq(line, "about")) {
        cmd_about();
        return;
    }

    if (str_eq(line, "ps")) {
        myos_ps();
        return;
    }

    if (str_eq(line, "threads")) {
        myos_threads();
        return;
    }

    if (str_eq(line, "uthreads")) {
        myos_uthreads();
        return;
    }

    if (str_eq(line, "cpus")) {
        myos_cpus();
        return;
    }

    if (str_eq(line, "smpbalance")) {
        myos_smp_balance();
        return;
    }

    if (str_eq(line, "smpbench")) {
        write_str("\n[smpbench] starting spin.elf (long-lived child runner)...\n");
        long pid = myos_exec("spin.elf");
        if (pid < 0) {
            write_str("[smpbench] exec failed rc=");
            write_rc(pid);
            write_str(" (likely process table full)\n");
            return;
        }
        write_str("[smpbench] child pid=");
        write_dec((unsigned long)pid);
        write_str("\n");
        for (int i = 0; i < 3; i++) {
            myos_yield();
        }
        myos_smp_balance();
        myos_threads();
        write_str("[smpbench] done (re-run smpbalance; use kill to stop spin.elf)\n");
        return;
    }

    if (str_eq(line, "killall")) {
        write_str("\nusage: killall <name>\n");
        return;
    }

    if (str_starts(line, "killall ")) {
        const char *arg = line + 8;
        skip_spaces(&arg);
        if (*arg == '\0') {
            write_str("\nusage: killall <name>\n");
            return;
        }
        long rc = myos_killall_name(arg);
        write_str("\nkillall \"");
        write_str(arg);
        write_str("\" killed=");
        if (rc < 0) {
            write_rc(rc);
        } else {
            write_dec((unsigned long)rc);
        }
        write_str("\n");
        return;
    }

    if (str_starts(line, "kill ")) {
        const char *arg = line + 5;
        skip_spaces(&arg);
        unsigned long pid = 0;
        if (parse_u32(arg, &pid) != 0 || pid == 0) {
            write_str("\nusage: kill <pid>\n");
            return;
        }
        long rc = myos_kill((long)pid);
        write_str("\nkill ");
        write_dec(pid);
        if (rc == 0) {
            write_str(" ok\n");
        } else {
            write_str(" rc=");
            write_rc(rc);
            write_str("\n");
        }
        return;
    }

    if (str_eq(line, "ports")) {
        myos_msg_ports();
        return;
    }

    if (str_eq(line, "ping")) {
        long rc = myos_msg_ping();
        if (rc < 0) {
            write_str("\nping failed\n");
        }
        return;
    }

    if (str_eq(line, "ipcmode")) {
        long m = myos_ipc_bump_mode(-1);
        write_str("\nipc bump mode: ");
        write_str(m == 1 ? "on\n" : "off\n");
        return;
    }

    if (str_starts(line, "ipcmode ")) {
        const char *arg = line + 8;
        skip_spaces(&arg);
        long set = -1;
        if (str_eq(arg, "on")) {
            set = 1;
        } else if (str_eq(arg, "off")) {
            set = 0;
        }
        if (set < 0) {
            write_str("\nusage: ipcmode [on|off]\n");
            return;
        }
        long m = myos_ipc_bump_mode(set);
        write_str("\nipc bump mode: ");
        write_str(m == 1 ? "on\n" : "off\n");
        return;
    }

    if (str_eq(line, "pingbench") || str_starts(line, "pingbench ")) {
        unsigned long n = 50;
        if (str_starts(line, "pingbench ")) {
            const char *arg = line + 10;
            skip_spaces(&arg);
            if (parse_u32(arg, &n) != 0 || n == 0) {
                write_str("\nusage: pingbench [N]\n");
                return;
            }
        }
        unsigned long ok = 0;
        for (unsigned long i = 0; i < n; i++) {
            if (myos_msg_ping_flags(MYOS_MSG_PING_SILENT) == 0) {
                ok++;
            }
        }
        write_str("\npingbench: ok=");
        write_dec(ok);
        write_str(" total=");
        write_dec(n);
        write_str("\n");
        return;
    }

    if (str_eq(line, "capsmoke")) {
        long cap = myos_cap_create_port();
        if (cap < 0) {
            write_str("\ncapsmoke: create failed\n");
            return;
        }

        const char *payload = "cap-ok";
        if (myos_cap_send(cap, payload, 6) != 0) {
            write_str("\ncapsmoke: send failed\n");
            return;
        }

        struct myos_msg m;
        if (myos_cap_recv(cap, &m, 0) != 0) {
            write_str("\ncapsmoke: recv failed\n");
            return;
        }
        myos_cap_close(cap);
        write_str("\ncapsmoke: ok cap=");
        write_dec((unsigned long)cap);
        write_str("\n");
        return;
    }

    if (str_eq(line, "capdiag") || str_starts(line, "capdiag stress ") ||
        str_starts(line, "capdiag grantstress ")) {
        unsigned long rounds = 1;
        int grantstress = 0;
        if (str_starts(line, "capdiag stress ")) {
            const char *arg = line + 15;
            skip_spaces(&arg);
            if (parse_u32(arg, &rounds) != 0 || rounds == 0) {
                write_str("\nusage: capdiag stress <N>\n");
                return;
            }
        } else if (str_starts(line, "capdiag grantstress ")) {
            const char *arg = line + 20;
            skip_spaces(&arg);
            if (parse_u32(arg, &rounds) != 0 || rounds == 0) {
                write_str("\nusage: capdiag grantstress <N>\n");
                return;
            }
            grantstress = 1;
        }

        unsigned long pass = 0;
        unsigned long fail = 0;

        write_str("\n[capdiag] start rounds=");
        write_dec(rounds);
        write_str("\n");

        for (unsigned long r = 0; r < rounds; r++) {
            if (rounds > 1) {
                write_str("[round ");
                write_dec(r + 1);
                write_str("]\n");
            }

            long cap = myos_cap_create_port();
            if (cap >= 0) {
                if (rounds == 1) {
                    write_str("[PASS] create slot=");
                    write_dec((unsigned long)cap);
                    write_str("\n");
                }
                pass++;
            } else {
                write_str("[FAIL] create rc=");
                write_rc(cap);
                write_str("\n");
                fail++;
            }

            if (cap >= 0) {
                long active_slot = cap;
                long granted_slot = -1;

                if (grantstress) {
                    long self_pid = myos_getpid();
                    if (self_pid <= 0) {
                        write_str("[FAIL] getpid rc=");
                        write_rc(self_pid);
                        write_str("\n");
                        fail++;
                    } else {
                        granted_slot = myos_cap_grant(cap, self_pid,
                            MYOS_CAP_RIGHT_SEND | MYOS_CAP_RIGHT_RECV);
                        if (granted_slot >= 0) {
                            if (rounds == 1) {
                                write_str("[PASS] grant-self slot=");
                                write_dec((unsigned long)granted_slot);
                                write_str("\n");
                            }
                            pass++;
                            active_slot = granted_slot;
                        } else {
                            write_str("[FAIL] grant-self rc=");
                            write_rc(granted_slot);
                            write_str("\n");
                            fail++;
                        }
                    }
                }

                const char *payload = "diag";
                long rc_send = myos_cap_send(active_slot, payload, 4);
                if (rc_send == 0) {
                    if (rounds == 1) {
                        write_str("[PASS] send rc=0\n");
                    }
                    pass++;
                } else {
                    write_str("[FAIL] send rc=");
                    write_rc(rc_send);
                    write_str("\n");
                    fail++;
                }

                struct myos_msg m;
                long rc_recv = myos_cap_recv(active_slot, &m, 0);
                if (rc_recv == 0 && m.size == 4 &&
                    m.data[0] == 'd' && m.data[1] == 'i' &&
                    m.data[2] == 'a' && m.data[3] == 'g') {
                    if (rounds == 1) {
                        write_str("[PASS] recv data=\"diag\"\n");
                    }
                    pass++;
                } else {
                    write_str("[FAIL] recv rc=");
                    write_rc(rc_recv);
                    write_str(" size=");
                    write_dec((unsigned long)m.size);
                    write_str("\n");
                    fail++;
                }

                long rc_recv_empty = myos_cap_recv(active_slot, &m, 0);
                if (rc_recv_empty == 1) {
                    if (rounds == 1) {
                        write_str("[PASS] recv-empty rc=1\n");
                    }
                    pass++;
                } else {
                    write_str("[FAIL] recv-empty rc=");
                    write_rc(rc_recv_empty);
                    write_str("\n");
                    fail++;
                }

                long rc_bad_grant = myos_cap_grant(cap, 999, 1);
                if (rc_bad_grant < 0) {
                    if (rounds == 1) {
                        write_str("[PASS] bad-grant rc=");
                        write_rc(rc_bad_grant);
                        write_str("\n");
                    }
                    pass++;
                } else {
                    write_str("[FAIL] bad-grant rc=");
                    write_rc(rc_bad_grant);
                    write_str("\n");
                    fail++;
                }

                if (granted_slot >= 0) {
                    long rc_close_granted = myos_cap_close(granted_slot);
                    if (rc_close_granted == 0) {
                        if (rounds == 1) {
                            write_str("[PASS] close-granted rc=0\n");
                        }
                        pass++;
                    } else {
                        write_str("[FAIL] close-granted rc=");
                        write_rc(rc_close_granted);
                        write_str("\n");
                        fail++;
                    }
                }

                long rc_close = myos_cap_close(cap);
                if (rc_close == 0) {
                    if (rounds == 1) {
                        write_str("[PASS] close rc=0\n");
                    }
                    pass++;
                } else {
                    write_str("[FAIL] close rc=");
                    write_rc(rc_close);
                    write_str("\n");
                    fail++;
                }
            }

            long rc_bad_send = myos_cap_send(999, "x", 1);
            if (rc_bad_send < 0) {
                if (rounds == 1) {
                    write_str("[PASS] bad-send rc=");
                    write_rc(rc_bad_send);
                    write_str("\n");
                }
                pass++;
            } else {
                write_str("[FAIL] bad-send rc=");
                write_rc(rc_bad_send);
                write_str("\n");
                fail++;
            }
        }

        write_str("[capdiag] done pass=");
        write_dec(pass);
        write_str(" fail=");
        write_dec(fail);
        write_str("\n");
        return;
    }

    if (str_starts(line, "capclose ")) {
        const char *arg = line + 9;
        skip_spaces(&arg);
        unsigned long slot = 0;
        if (parse_u32(arg, &slot) != 0) {
            write_str("\nusage: capclose <slot>\n");
            return;
        }
        long rc = myos_cap_close((long)slot);
        write_str("\ncapclose rc=");
        write_rc(rc);
        write_str("\n");
        return;
    }

    if (str_eq(line, "capnew")) {
        long cap = myos_cap_create_port();
        if (cap < 0) {
            write_str("\ncapnew failed rc=");
            write_dec((unsigned long)(0 - cap));
            write_str("\n");
            return;
        }
        write_str("\ncapnew slot=");
        write_dec((unsigned long)cap);
        write_str("\n");
        return;
    }

    if (str_starts(line, "capsend ")) {
        const char *args = line + 8;
        skip_spaces(&args);
        const char *sp = args;
        while (*sp && *sp != ' ') {
            sp++;
        }
        if (*sp == '\0') {
            write_str("\nusage: capsend <slot> <text>\n");
            return;
        }
        char slot_buf[16];
        int sl = (int)(sp - args);
        if (sl <= 0 || sl >= 16) {
            write_str("\nusage: capsend <slot> <text>\n");
            return;
        }
        for (int i = 0; i < sl; i++) {
            slot_buf[i] = args[i];
        }
        slot_buf[sl] = '\0';
        unsigned long slot = 0;
        if (parse_u32(slot_buf, &slot) != 0) {
            write_str("\nusage: capsend <slot> <text>\n");
            return;
        }
        const char *text = sp;
        skip_spaces(&text);
        if (*text == '\0') {
            write_str("\nusage: capsend <slot> <text>\n");
            return;
        }
        unsigned long len = str_len(text);
        if (len > 60) {
            len = 60;
        }
        long rc = myos_cap_send((long)slot, text, len);
        write_str("\ncapsend rc=");
        write_rc(rc);
        write_str("\n");
        return;
    }

    if (str_starts(line, "caprecv ")) {
        const char *args = line + 8;
        skip_spaces(&args);
        const char *sp = args;
        while (*sp && *sp != ' ') {
            sp++;
        }
        char slot_buf[16];
        int sl = (int)(sp - args);
        if (sl <= 0 || sl >= 16) {
            write_str("\nusage: caprecv <slot> [block]\n");
            return;
        }
        for (int i = 0; i < sl; i++) {
            slot_buf[i] = args[i];
        }
        slot_buf[sl] = '\0';
        unsigned long slot = 0;
        if (parse_u32(slot_buf, &slot) != 0) {
            write_str("\nusage: caprecv <slot> [block]\n");
            return;
        }
        long block = 0;
        if (*sp) {
            const char *b = sp;
            skip_spaces(&b);
            if (*b) {
                unsigned long bv = 0;
                if (parse_u32(b, &bv) != 0 || bv > 1) {
                    write_str("\nusage: caprecv <slot> [block]\n");
                    return;
                }
                block = (long)bv;
            }
        }

        struct myos_msg m;
        long rc = myos_cap_recv((long)slot, &m, block);
        if (rc != 0) {
            write_str("\ncaprecv rc=");
            write_rc(rc);
            write_str("\n");
            return;
        }

        write_str("\ncaprecv ok from=");
        write_dec((unsigned long)m.from);
        write_str(" size=");
        write_dec((unsigned long)m.size);
        write_str(" data=\"");
        unsigned long n = m.size;
        if (n > 60) {
            n = 60;
        }
        myos_write(1, m.data, n);
        write_str("\"\n");
        return;
    }

    if (str_starts(line, "capgrant ")) {
        const char *args = line + 9;
        skip_spaces(&args);

        const char *p1 = args;
        while (*p1 && *p1 != ' ') {
            p1++;
        }
        if (*p1 == '\0') {
            write_str("\nusage: capgrant <slot> <pid> <rights>\n");
            return;
        }
        char b1[16];
        int n1 = (int)(p1 - args);
        if (n1 <= 0 || n1 >= 16) {
            write_str("\nusage: capgrant <slot> <pid> <rights>\n");
            return;
        }
        for (int i = 0; i < n1; i++) b1[i] = args[i];
        b1[n1] = '\0';

        const char *p2 = p1;
        skip_spaces(&p2);
        const char *p3 = p2;
        while (*p3 && *p3 != ' ') {
            p3++;
        }
        if (*p3 == '\0') {
            write_str("\nusage: capgrant <slot> <pid> <rights>\n");
            return;
        }
        char b2[16];
        int n2 = (int)(p3 - p2);
        if (n2 <= 0 || n2 >= 16) {
            write_str("\nusage: capgrant <slot> <pid> <rights>\n");
            return;
        }
        for (int i = 0; i < n2; i++) b2[i] = p2[i];
        b2[n2] = '\0';

        const char *p4 = p3;
        skip_spaces(&p4);
        if (*p4 == '\0') {
            write_str("\nusage: capgrant <slot> <pid> <rights>\n");
            return;
        }

        unsigned long slot = 0, pid = 0, rights = 0;
        if (parse_u32(b1, &slot) != 0 || parse_u32(b2, &pid) != 0 || parse_u32(p4, &rights) != 0) {
            write_str("\nusage: capgrant <slot> <pid> <rights>\n");
            return;
        }

        long rc = myos_cap_grant((long)slot, (long)pid, (long)rights);
        if (rc < 0) {
            write_str("\ncapgrant rc=");
            write_str("-");
            write_dec((unsigned long)(0 - rc));
            write_str("\n");
            return;
        }
        write_str("\ncapgrant new_slot=");
        write_dec((unsigned long)rc);
        write_str("\n");
        return;
    }

    if (str_eq(line, "pingbench_ab") || str_starts(line, "pingbench_ab ")) {
        unsigned long n = 50;
        if (str_starts(line, "pingbench_ab ")) {
            const char *arg = line + 13;
            skip_spaces(&arg);
            if (parse_u32(arg, &n) != 0 || n == 0) {
                write_str("\nusage: pingbench_ab [N]\n");
                return;
            }
        }

        long orig = myos_ipc_bump_mode(-1);
        unsigned long ok_off = 0;
        unsigned long ok_on = 0;

        myos_ipc_bump_mode(0);
        for (unsigned long i = 0; i < n; i++) {
            if (myos_msg_ping_flags(MYOS_MSG_PING_SILENT) == 0) {
                ok_off++;
            }
        }

        myos_ipc_bump_mode(1);
        for (unsigned long i = 0; i < n; i++) {
            if (myos_msg_ping_flags(MYOS_MSG_PING_SILENT) == 0) {
                ok_on++;
            }
        }

        if (orig == 0 || orig == 1) {
            myos_ipc_bump_mode(orig);
        }

        write_str("\npingbench_ab off=");
        write_dec(ok_off);
        write_str("/");
        write_dec(n);
        write_str(" on=");
        write_dec(ok_on);
        write_str("/");
        write_dec(n);
        write_str("\n");
        return;
    }

    if (str_starts(line, "msg ")) {
        const char *args = line + 4;
        skip_spaces(&args);
        if (*args == '\0') {
            write_str("\nusage: msg [port] <text>\n");
            return;
        }

        char word[16];
        int wi = 0;
        while (args[wi] && args[wi] != ' ' && wi < 15) {
            word[wi] = args[wi];
            wi++;
        }
        word[wi] = '\0';

        const char *port = "msgd";
        const char *text = args;
        if (wi > 0 && args[wi] == ' ' && myos_port_lookup(word) > 0) {
            port = word;
            text = args + wi;
            skip_spaces(&text);
        }

        if (*text == '\0') {
            write_str("\nusage: msg [port] <text>\n");
            return;
        }

        unsigned long len = str_len(text);
        if (len > 60) {
            len = 60;
        }
        long rc = myos_msg_send_name(port, text, len);
        if (rc < 0) {
            write_str("\nmsg send failed\n");
        }
        return;
    }

    if (str_eq(line, "yield")) {
        for (int i = 0; i < 5; i++) {
            myos_yield();
        }
        write_str("\nyield OK\n");
        return;
    }

    if (str_starts(line, "echo ")) {
        write_str("\n");
        write_str(line + 5);
        return;
    }

    if (str_starts(line, "exec")) {
        const char *name = "hello.elf";
        const char *args = line + 4;
        skip_spaces(&args);
        if (*args != '\0') {
            name = args;
        }

        long pid = myos_exec(name);
        if (pid < 0) {
            write_str("\nexec failed: ");
            write_str(name);
            write_str(" rc=");
            write_rc(pid);
            write_str("\n");
            return;
        }

        write_str("\nStarted process ");
        char num[12];
        int n = 0;
        unsigned long v = (unsigned long)pid;
        if (v == 0) {
            num[n++] = '0';
        } else {
            char tmp[12];
            int t = 0;
            while (v > 0) {
                tmp[t++] = (char)('0' + (v % 10));
                v /= 10;
            }
            while (t > 0) {
                num[n++] = tmp[--t];
            }
        }
        num[n] = '\0';
        write_str(num);
        write_str(" (");
        write_str(name);
        write_str(")\n");
        return;
    }

    if (str_eq(line, "spin")) {
        long pid = myos_exec("spin.elf");
        if (pid < 0) {
            write_str("\nexec failed: spin.elf rc=");
            write_rc(pid);
            write_str("\n");
            return;
        }
        write_str("\nStarted process ");
        write_dec((unsigned long)pid);
        write_str(" (spin.elf)\n");
        return;
    }

    write_str("\nUnknown command: ");
    write_str(line);
    write_str("\nType 'help' for commands.");
}

int main(void) {
    write_str("MyOS userland shell started\n");

    char line[128];

    for (;;) {
        write_prompt();
        if (read_line(line, (int)sizeof(line)) < 0) {
            break;
        }
        run_command(line);
    }

    return 0;
}
