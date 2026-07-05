/*
 * MyOS userland program template.
 *
 * 1. Copy this file:  cp template.c foo.c
 * 2. Edit foo.c
 * 3. Add foo to PROGRAMS in user/programs.mk
 * 4. make && make run
 * 5. In shell: exec foo.elf
 *
 * See docs/USERLAND.md for the full guide (incl. multithreading).
 */
#include "myos.h"
#include "myos_util.h"

/* Uncomment for a multi-threaded skeleton:
#include "myos_thread.h"

void worker(uint64_t id) {
    myos_write_str("worker ");
    myos_write_dec(id);
    myos_write_str("\n");
    myos_exit(0);   // required — do not return from worker
}

int main(void) {
    long t = myos_thread_spawn(worker, 1);
    if (t < 0) return 1;
    myos_thread_join(t);
    return 0;
}
*/

int main(void) {
    myos_write_str("template: replace me\n");
    return 0;
}
