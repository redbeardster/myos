#ifndef KBDD_H
#define KBDD_H

#include <stdint.h>

int kbdd_start(void);
uint32_t kbdd_thread_id(void);

/* Called from IRQ after a scancode is queued. */
void kbdd_irq_notify(void);

/* Request one character via msgport IPC (MSG_KBD_WAIT → MSG_KBD_CHAR). */
int kbdd_request_char(void);

#endif
