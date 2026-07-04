#include "elf.h"

#include "memory.h"
#include "vmm.h"

#include <stdint.h>

static void memset_local(void *dst, uint8_t val, size_t n) {
    uint8_t *p = (uint8_t *)dst;
    while (n--) {
        *p++ = val;
    }
}

static void memcpy_local(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) {
        *d++ = *s++;
    }
}

static uint64_t phdr_pte_flags(uint32_t p_flags) {
    uint64_t flags = PTE_USER;
    if (p_flags & PF_W) {
        flags |= PTE_WRITE;
    }
    return flags;
}

int elf_validate(const void *data, size_t size) {
    if (!data || size < sizeof(struct elf64_ehdr)) {
        return -1;
    }

    const struct elf64_ehdr *eh = (const struct elf64_ehdr *)data;

    if (*(const uint32_t *)eh->e_ident != ELF_MAGIC) {
        return -1;
    }
    if (eh->e_ident[4] != ELFCLASS64 || eh->e_ident[5] != ELFDATA2LSB) {
        return -1;
    }
    if (eh->e_type != ET_EXEC || eh->e_machine != EM_X86_64) {
        return -1;
    }
    if (eh->e_phoff == 0 || eh->e_phnum == 0) {
        return -1;
    }
    if (eh->e_phoff + (uint64_t)eh->e_phnum * sizeof(struct elf64_phdr) > size) {
        return -1;
    }

    return 0;
}

int elf_load(const void *data, size_t size, uint64_t cr3, struct elf_load_info *info) {
    if (!info || !cr3 || elf_validate(data, size) != 0) {
        return -1;
    }

    const struct elf64_ehdr *eh = (const struct elf64_ehdr *)data;
    const struct elf64_phdr *phdrs = (const struct elf64_phdr *)((const uint8_t *)data + eh->e_phoff);

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const struct elf64_phdr *ph = &phdrs[i];
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) {
            continue;
        }

        if (ph->p_offset + ph->p_filesz > size) {
            return -2;
        }

        uint64_t seg_start = ph->p_vaddr & ~(uint64_t)(PAGE_SIZE - 1);
        uint64_t seg_end = (ph->p_vaddr + ph->p_memsz + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
        uint64_t flags = phdr_pte_flags(ph->p_flags);

        for (uint64_t va = seg_start; va < seg_end; va += PAGE_SIZE) {
            void *page = alloc_page();
            if (!page) {
                return -3;
            }

            memset_local(page, 0, PAGE_SIZE);

            if (vmm_map(cr3, va, virt_to_phys(page), flags) != 0) {
                free_page(page);
                return -4;
            }

            uint64_t page_off = 0;
            if (va < ph->p_vaddr) {
                page_off = ph->p_vaddr - va;
            }

            uint64_t dst_start = ph->p_vaddr > va ? ph->p_vaddr : va;
            uint64_t file_end = ph->p_vaddr + ph->p_filesz;
            if (dst_start < file_end) {
                uint64_t copy_len = file_end - dst_start;
                if (copy_len > PAGE_SIZE - page_off) {
                    copy_len = PAGE_SIZE - page_off;
                }
                uint64_t src_off = ph->p_offset + (dst_start - ph->p_vaddr);
                memcpy_local((uint8_t *)page + page_off, (const uint8_t *)data + src_off, (size_t)copy_len);
            }
        }
    }

    info->entry = eh->e_entry;
    return 0;
}
