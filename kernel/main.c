#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <limine.h>

#include "console.h"
#include "cpu.h"
#include "exec.h"
#include "gdt.h"
#include "interrupt.h"
#include "keyboard.h"
#include "lwkt.h"
#include "memory.h"
#include "msgport.h"
#include "proc.h"
#include "syscall.h"
#include "vmm.h"

__attribute__((used, section(".limine_requests")))
static volatile uint64_t limine_base_revision[3] = {
    0xf9562b2d5c95a6c8, 0x6a7b384944536bdc, 3
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id = { LIMINE_COMMON_MAGIC, 0x67cf3d9d378a806f, 0xe304acdfc50c3c62 },
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_hhdm_request hhdm_request = {
    .id = { LIMINE_COMMON_MAGIC, 0x48dcf1cb8ad2b852, 0x63984e959a98244b },
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
volatile struct limine_module_request module_request = {
    .id = { LIMINE_COMMON_MAGIC, 0x3e7e279702be32af, 0xca1c4f3bd1280cee },
    .revision = 0,
    .response = 0,
    .internal_module_count = 0,
    .internal_modules = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = { LIMINE_COMMON_MAGIC, 0x9d5827dcd881dd75, 0xa3148604f6fab11b },
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_smp_request smp_request = {
    .id = LIMINE_SMP_REQUEST,
    .revision = 0
};

__attribute__((used, section(".limine_requests_start")))
static volatile uint64_t limine_requests_start_marker[4] = {
    0xf6b8f4b39de7d1ae, 0xfab91a6940fcb9cf,
    0x785c6ed015d3e316, 0x181e920a7852b9d9
};

__attribute__((used, section(".limine_requests_end")))
static volatile uint64_t limine_requests_end_marker[2] = {
    0xadc0e0531bb10d03, 0x9572709f31764c62
};

static void hcf(void) {
    for (;;) {
        __asm__ volatile("hlt");
    }
}

void kmain(void) {
    if (LIMINE_BASE_REVISION_SUPPORTED == false) {
        hcf();
    }

    if (framebuffer_request.response == NULL ||
        framebuffer_request.response->framebuffer_count < 1) {
        hcf();
    }

    if (memmap_request.response == NULL || hhdm_request.response == NULL) {
        hcf();
    }

    struct limine_framebuffer *fb = framebuffer_request.response->framebuffers[0];
    console_init(fb);

    gdt_init();
    memory_init(memmap_request.response, hhdm_request.response->offset);
    vmm_init();
    proc_init();
    cpu_init_bsp();
    syscall_init();
    keyboard_init();
    idt_init();
    pic_init();
    interrupts_enable();

    console_setcolor(COLOR_WHITE, COLOR_BLUE);
    console_writestring("============================================================\n");
    console_writestring("        MyOS Kernel v2.0 (x86-64 / Limine / LWKT / SMP)     \n");
    console_writestring("============================================================\n\n");
    console_setcolor(COLOR_WHITE, COLOR_BLACK);

    console_writestring("System initialized successfully!\n");

    lwkt_init();

    if (msgd_start() < 0) {
        console_writestring("Failed to start msgd kernel thread\n");
    } else {
        console_writestring("msgd kernel message thread started (LWKT id ");
        console_write_dec(msgd_thread_id());
        console_writestring(")\n");
    }

    if (exec_start_shell() < 0) {
        console_writestring("Failed to start userland shell\n");
        hcf();
    }

    console_writestring("Userland shell started (ring 3)\n");
    memory_print_info();

    if (smp_request.response) {
        smp_init(smp_request.response);
    }

    smp_release_aps();
    lwkt_bootstrap_first();

    hcf();
}
