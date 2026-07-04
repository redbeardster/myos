#ifndef USER_H
#define USER_H

#include <stdint.h>

#include "myos_abi.h"

void *user_page_alloc(void);
void user_free_page(void *user_addr);
void user_enter(uint64_t rip, uint64_t rsp, uint64_t *save_kernel_rsp);

#endif
