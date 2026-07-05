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

static void write_str(const char *s) {
    myos_write(1, s, str_len(s));
}

static void write_prompt(void) {
    write_str("\nMyOS> ");
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
    write_str("  ps              - list processes (CR3, uthreads)\n");
    write_str("  uthreads        - list uthreads (slot, proc, lwkt)\n");
    write_str("  cpus            - SMP CPU status (per-CPU scheduler)\n");
    write_str("  threads         - list LWKT scheduler threads\n");
    write_str("  ports           - list named msgports\n");
    write_str("  msg [port] text - send to msgport (default msgd)\n");
    write_str("  ping            - msgport ping/pong with msgd\n");
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
