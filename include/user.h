#ifndef USER_H
#define USER_H

#include <stdint.h>

#include "myos_abi.h"
#include "proc.h"

void *user_page_alloc(void);
void user_free_page(void *user_addr);
int user_stack_alloc(struct proc *p, uint64_t *rsp_out, uint64_t *base_out);
void user_stack_free(struct proc *p, uint64_t stack_base);
void user_enter(uint64_t rip, uint64_t rsp, uint64_t arg, uint64_t user_rax,
                uint64_t *save_kernel_rsp);

#endif
