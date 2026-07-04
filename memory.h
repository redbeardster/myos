#ifndef MEMORY_H
#define MEMORY_H

#include <stddef.h>
#include <stdint.h>

#define PAGE_SIZE 4096

struct limine_memmap_response;

void memory_init(struct limine_memmap_response *memmap, uint64_t hhdm_offset);
uint64_t memory_hhdm(void);

void *phys_to_virt(uint64_t phys);
uint64_t virt_to_phys(void *virt);

void *alloc_page(void);
void free_page(void *page);
void *alloc_pages(size_t count);
void free_pages(void *page, size_t count);

uint64_t memory_total_pages(void);
uint64_t memory_free_pages(void);
void memory_print_info(void);

#endif
