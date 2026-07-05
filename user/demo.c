/*
 * Demo userland program — built and embedded by default.
 * Run from shell: exec demo.elf
 */
#include "myos.h"

static void write_str(const char *s) {
    unsigned long n = 0;
    while (s[n]) {
        n++;
    }
    myos_write(1, s, n);
}

int main(void) {
    write_str("demo.elf: MyOS userland works!\n");
    return 0;
}
