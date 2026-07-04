#include "exec.h"

#include "console.h"
#include "elf.h"
#include "memory.h"
#include "myos_abi.h"
#include "proc.h"
#include "uthread.h"
#include "vmm.h"

#include <limine.h>
#include <stddef.h>
#include <stdint.h>

extern uint8_t _binary_user_hello_elf_start[];
extern uint8_t _binary_user_hello_elf_end[];
extern uint8_t _binary_user_shell_elf_start[];
extern uint8_t _binary_user_shell_elf_end[];

extern volatile struct limine_module_request module_request;

static int path_ends_with(const char *path, const char *suffix) {
    if (!path || !suffix) {
        return 0;
    }

    int plen = 0;
    int slen = 0;
    while (path[plen]) {
        plen++;
    }
    while (suffix[slen]) {
        slen++;
    }
    if (slen > plen) {
        return 0;
    }

    for (int i = 0; i < slen; i++) {
        if (path[plen - slen + i] != suffix[i]) {
            return 0;
        }
    }
    return 1;
}

static const char *base_name(const char *path) {
    const char *base = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/') {
            base = p + 1;
        }
    }
    return base;
}

static int spawn_from_blob(const void *elf, size_t size, const char *name, uint32_t flags) {
    if (!elf || size == 0) {
        return -1;
    }
    return exec_spawn_elf(elf, size, name, flags);
}

int exec_spawn_elf(const void *elf, size_t size, const char *name, uint32_t flags) {
    struct elf_load_info info;
    int is_shell = (flags & EXEC_FLAG_SHELL) != 0;

    if (elf_validate(elf, size) != 0) {
        return -1;
    }

    if (is_shell) {
        proc_kill_all();
    } else {
        proc_kill_children();
    }

    uint64_t cr3 = vmm_aspace_create();
    if (!cr3) {
        return -7;
    }

    if (elf_load(elf, size, cr3, &info) != 0) {
        vmm_aspace_destroy(cr3);
        return -2;
    }

    void *stack_page = alloc_page();
    if (!stack_page) {
        vmm_aspace_destroy(cr3);
        return -3;
    }

    uint64_t stack_map = MYOS_USER_STACK_TOP - PAGE_SIZE;
    if (vmm_map(cr3, stack_map, virt_to_phys(stack_page), PTE_WRITE) != 0) {
        free_page(stack_page);
        vmm_aspace_destroy(cr3);
        return -4;
    }

    struct proc *p = proc_create(name, cr3, info.entry, MYOS_USER_STACK_TOP - 16, is_shell);
    if (!p) {
        vmm_aspace_destroy(cr3);
        return -5;
    }

    uint32_t prio = is_shell ? LWKT_PRIO_SHELL : LWKT_PRIO_NORMAL;
    struct uthread *u = uthread_spawn_in_proc(p, info.entry, MYOS_USER_STACK_TOP - 16, prio);
    if (!u) {
        proc_destroy(p);
        return -6;
    }

    return (int)p->pid;
}

void exec_list_modules(void) {
    if (!module_request.response || module_request.response->module_count == 0) {
        console_writestring("\nNo modules loaded by Limine\n");
        return;
    }

    console_writestring("\nLimine modules:\n");
    for (uint64_t i = 0; i < module_request.response->module_count; i++) {
        struct limine_file *m = module_request.response->modules[i];
        if (!m) {
            continue;
        }
        console_writestring("  ");
        if (m->path) {
            console_writestring(m->path);
        } else {
            console_writestring("(unknown)");
        }
        console_writestring("  size=");
        console_write_dec(m->size);
        console_putchar('\n');
    }
}

int exec_spawn_module(const char *name, uint32_t flags) {
    if (module_request.response) {
        for (uint64_t i = 0; i < module_request.response->module_count; i++) {
            struct limine_file *m = module_request.response->modules[i];
            if (!m || !m->path || !m->address || m->size == 0) {
                continue;
            }

            if (path_ends_with(m->path, name)) {
                const char *pname = base_name(m->path);
                return exec_spawn_elf(m->address, m->size, pname, flags);
            }
        }
    }

    if (path_ends_with(name, "shell.elf")) {
        size_t size = (size_t)(_binary_user_shell_elf_end - _binary_user_shell_elf_start);
        if (size > 0) {
            return spawn_from_blob(_binary_user_shell_elf_start, size, "shell.elf", flags);
        }
    }

    if (path_ends_with(name, "hello.elf")) {
        size_t size = (size_t)(_binary_user_hello_elf_end - _binary_user_hello_elf_start);
        if (size > 0) {
            return spawn_from_blob(_binary_user_hello_elf_start, size, "hello.elf", flags);
        }
    }

    return -1;
}

int exec_start_shell(void) {
    return exec_spawn_module("shell.elf", EXEC_FLAG_SHELL);
}
