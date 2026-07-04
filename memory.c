#include "memory.h"

#include "console.h"

#include <limine.h>
#include <stdint.h>

#define MAX_PHYS_BYTES (256ULL * 1024 * 1024)
#define MAX_PHYS_PAGES (MAX_PHYS_BYTES / PAGE_SIZE)

extern char _kernel_start[];
extern char _kernel_end[];

static uint8_t page_bitmap[MAX_PHYS_PAGES / 8];
static uint64_t hhdm;
static uint64_t total_pages;
static uint64_t free_count;

static void bitmap_set(uint64_t page) {
    if (page >= MAX_PHYS_PAGES) {
        return;
    }
    page_bitmap[page / 8] |= (uint8_t)(1U << (page % 8));
}

static void bitmap_clear(uint64_t page) {
    if (page >= MAX_PHYS_PAGES) {
        return;
    }
    page_bitmap[page / 8] &= (uint8_t)~(1U << (page % 8));
}

static int bitmap_test(uint64_t page) {
    if (page >= MAX_PHYS_PAGES) {
        return 1;
    }
    return (page_bitmap[page / 8] >> (page % 8)) & 1;
}

static void reserve_phys(uint64_t base, uint64_t length) {
    uint64_t start = base / PAGE_SIZE;
    uint64_t end = (base + length + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint64_t p = start; p < end; p++) {
        if (!bitmap_test(p)) {
            bitmap_set(p);
            if (free_count > 0) {
                free_count--;
            }
        }
    }
}

static int memmap_usable(uint64_t type) {
    return type == LIMINE_MEMMAP_USABLE ||
           type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE;
}

void memory_init(struct limine_memmap_response *memmap, uint64_t hhdm_offset) {
    hhdm = hhdm_offset;
    total_pages = 0;
    free_count = 0;

    for (size_t i = 0; i < sizeof(page_bitmap); i++) {
        page_bitmap[i] = 0xFF;
    }

    if (!memmap || !memmap->entries) {
        return;
    }

    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *e = memmap->entries[i];
        if (!e || !memmap_usable(e->type)) {
            continue;
        }

        uint64_t start = e->base / PAGE_SIZE;
        uint64_t end = (e->base + e->length) / PAGE_SIZE;
        for (uint64_t p = start; p < end && p < MAX_PHYS_PAGES; p++) {
            if (bitmap_test(p)) {
                bitmap_clear(p);
                total_pages++;
                free_count++;
            }
        }
    }

    reserve_phys((uint64_t)(uintptr_t)&_kernel_start - hhdm,
                 (uint64_t)(_kernel_end - _kernel_start));
    reserve_phys((uint64_t)(uintptr_t)page_bitmap - hhdm, sizeof(page_bitmap));
}

uint64_t memory_hhdm(void) {
    return hhdm;
}

void *phys_to_virt(uint64_t phys) {
    return (void *)(phys + hhdm);
}

uint64_t virt_to_phys(void *virt) {
    return (uint64_t)(uintptr_t)virt - hhdm;
}

void *alloc_page(void) {
    for (uint64_t p = 0; p < MAX_PHYS_PAGES; p++) {
        if (!bitmap_test(p)) {
            bitmap_set(p);
            free_count--;
            return phys_to_virt(p * PAGE_SIZE);
        }
    }
    return NULL;
}

void free_page(void *page) {
    if (!page) {
        return;
    }
    uint64_t phys = virt_to_phys(page);
    uint64_t p = phys / PAGE_SIZE;
    if (p >= MAX_PHYS_PAGES || !bitmap_test(p)) {
        return;
    }
    bitmap_clear(p);
    free_count++;
}

void *alloc_pages(size_t count) {
    if (count == 0) {
        return NULL;
    }

    for (uint64_t start = 0; start < MAX_PHYS_PAGES; start++) {
        int ok = 1;
        for (size_t i = 0; i < count; i++) {
            if (start + i >= MAX_PHYS_PAGES || bitmap_test(start + i)) {
                ok = 0;
                break;
            }
        }
        if (!ok) {
            continue;
        }

        for (size_t i = 0; i < count; i++) {
            bitmap_set(start + i);
            free_count--;
        }
        return phys_to_virt(start * PAGE_SIZE);
    }
    return NULL;
}

void free_pages(void *page, size_t count) {
    if (!page || count == 0) {
        return;
    }
    uint64_t start = virt_to_phys(page) / PAGE_SIZE;
    for (size_t i = 0; i < count; i++) {
        uint64_t p = start + i;
        if (p < MAX_PHYS_PAGES && bitmap_test(p)) {
            bitmap_clear(p);
            free_count++;
        }
    }
}

uint64_t memory_total_pages(void) {
    return total_pages;
}

uint64_t memory_free_pages(void) {
    return free_count;
}

void memory_print_info(void) {
    console_writestring("\nPhysical memory:\n");
    console_writestring("  Total pages: ");
    console_write_dec(total_pages);
    console_writestring("\n  Free pages:  ");
    console_write_dec(free_count);
    console_writestring("\n  Page size:   ");
    console_write_dec(PAGE_SIZE);
    console_writestring(" bytes\n  HHDM offset: 0x");
    console_write_hex(hhdm);
    console_putchar('\n');
}
