#include "myos_util.h"

int main(void) {
    myos_write_str("spin.elf: started (Ctrl+C/kill process from shell controls)\n");

    for (;;) {
        for (unsigned long i = 0; i < 50000; i++) {
            myos_yield();
        }
    }
}
