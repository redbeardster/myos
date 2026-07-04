#include "myos.h"

int main(void) {
    const char msg[] = "Hello from MyOS ELF userland!\n";
    myos_write(1, msg, sizeof(msg) - 1);

    char *page = (char *)myos_alloc_page();
    if (page) {
        const char ok[] = "heap page OK\n";
        for (unsigned long i = 0; i < sizeof(ok) - 1; i++) {
            page[i] = ok[i];
        }
        myos_write(1, page, sizeof(ok) - 1);
        myos_free_page(page);
    }

    for (int i = 0; i < 10; i++) {
        myos_yield();
    }

    return 0;
}
