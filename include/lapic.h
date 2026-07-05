#ifndef LAPIC_H
#define LAPIC_H

#include <stdint.h>

#define LAPIC_TIMER_VECTOR 64
#define LAPIC_IPI_RESCHED_VECTOR 65

void lapic_init(void);
void lapic_timer_start(void);
void lapic_timer_stop(void);
void lapic_eoi(void);
void lapic_ipi_send(uint32_t dest_lapic_id, uint8_t vector);
void lapic_ipi_reschedule_others(void);
uint32_t lapic_id(void);

#endif
