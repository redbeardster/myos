#ifndef MYOS_UTIL_H
#define MYOS_UTIL_H

#include "myos.h"

static inline unsigned long myos_strlen(const char *s) {
    unsigned long n = 0;
    if (!s) {
        return 0;
    }
    while (s[n]) {
        n++;
    }
    return n;
}

static inline void myos_write_str(const char *s) {
    if (!s) {
        return;
    }
    myos_write(1, s, myos_strlen(s));
}

static inline void myos_write_char(char c) {
    myos_write(1, &c, 1);
}

static inline void myos_write_dec(unsigned long n) {
    char buf[20];
    int i = 0;

    if (n == 0) {
        buf[i++] = '0';
    } else {
        while (n > 0) {
            buf[i++] = (char)('0' + (n % 10));
            n /= 10;
        }
    }

    while (i > 0) {
        myos_write(1, &buf[--i], 1);
    }
}

static inline void myos_write_hex(unsigned long n) {
    static const char hex[] = "0123456789abcdef";
    char buf[18];
    int i = 0;

    buf[i++] = '0';
    buf[i++] = 'x';
    if (n == 0) {
        buf[i++] = '0';
    } else {
        char tmp[16];
        int j = 0;
        while (n > 0) {
            tmp[j++] = hex[n & 0xF];
            n >>= 4;
        }
        while (j > 0) {
            buf[i++] = tmp[--j];
        }
    }

    myos_write(1, buf, (unsigned long)i);
}

#endif
