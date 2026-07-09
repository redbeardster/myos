#ifndef EXEC_H
#define EXEC_H

#include <stddef.h>
#include <stdint.h>

#define EXEC_FLAG_SHELL  1

int exec_spawn_elf(const void *elf, size_t size, const char *name, uint32_t flags,
                   uint64_t exec_arg0, uint64_t exec_arg1);
int exec_spawn_module(const char *name, uint32_t flags,
                      uint64_t exec_arg0, uint64_t exec_arg1);
int exec_start_shell(void);
void exec_list_modules(void);

#endif
