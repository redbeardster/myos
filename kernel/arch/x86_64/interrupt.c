#include "interrupt.h"
#include "console.h"
#include "cpu.h"
#include "io.h"
#include "keyboard.h"
#include "lapic.h"
#include "lwkt.h"

#define PIC1_COMMAND 0x20
#define PIC1_DATA    0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA    0xA1
#define PIC_EOI      0x20

#define PIT_COMMAND  0x43
#define PIT_CHANNEL0 0x40
#define PIT_FREQUENCY 1193180
#define TICK_FREQUENCY 100

#define IRQ_TIMER 0
#define IRQ_KEYBOARD 1

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct idt_entry idt[256];
struct idt_ptr idtp;

extern void isr0(void);
extern void isr1(void);
extern void isr2(void);
extern void isr3(void);
extern void isr4(void);
extern void isr5(void);
extern void isr6(void);
extern void isr7(void);
extern void isr8(void);
extern void isr9(void);
extern void isr10(void);
extern void isr11(void);
extern void isr12(void);
extern void isr13(void);
extern void isr14(void);
extern void isr15(void);
extern void isr16(void);
extern void isr17(void);
extern void isr18(void);
extern void isr19(void);
extern void isr20(void);
extern void isr21(void);
extern void isr22(void);
extern void isr23(void);
extern void isr24(void);
extern void isr25(void);
extern void isr26(void);
extern void isr27(void);
extern void isr28(void);
extern void isr29(void);
extern void isr30(void);
extern void isr31(void);
extern void isr32(void);
extern void isr33(void);
extern void isr34(void);
extern void isr35(void);
extern void isr36(void);
extern void isr37(void);
extern void isr38(void);
extern void isr39(void);
extern void isr40(void);
extern void isr41(void);
extern void isr42(void);
extern void isr43(void);
extern void isr44(void);
extern void isr45(void);
extern void isr46(void);
extern void isr47(void);
extern void isr64(void);
extern void isr65(void);

extern void isr128(void);
extern void idt_load(uint64_t ptr);

static void (*isr_table[48])(void) = {
    isr0, isr1, isr2, isr3, isr4, isr5, isr6, isr7,
    isr8, isr9, isr10, isr11, isr12, isr13, isr14, isr15,
    isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23,
    isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31,
    isr32, isr33, isr34, isr35, isr36, isr37, isr38, isr39,
    isr40, isr41, isr42, isr43, isr44, isr45, isr46, isr47,
};

static volatile int timer_ticks;
static volatile int timer_enabled;
static volatile int timer_switch_count;

static void idt_set_gate(uint8_t num, void (*handler)(void)) {
    uint64_t addr = (uint64_t)handler;
    idt[num].offset_low = (uint16_t)(addr & 0xFFFF);
    idt[num].offset_mid = (uint16_t)((addr >> 16) & 0xFFFF);
    idt[num].offset_high = (uint32_t)((addr >> 32) & 0xFFFFFFFF);
    idt[num].selector = 0x08;
    idt[num].ist = 0;
    idt[num].type_attr = 0x8E;
    idt[num].zero = 0;
}

static void idt_set_gate_user(uint8_t num, void (*handler)(void)) {
    uint64_t addr = (uint64_t)handler;
    idt[num].offset_low = (uint16_t)(addr & 0xFFFF);
    idt[num].offset_mid = (uint16_t)((addr >> 16) & 0xFFFF);
    idt[num].offset_high = (uint32_t)((addr >> 32) & 0xFFFFFFFF);
    idt[num].selector = 0x08;
    idt[num].ist = 0;
    idt[num].type_attr = 0xEE;
    idt[num].zero = 0;
}

void pic_init(void) {
    outb(PIC1_COMMAND, 0x11);
    outb(PIC1_DATA, 0x20);
    outb(PIC1_DATA, 0x04);
    outb(PIC1_DATA, 0x01);
    outb(PIC1_DATA, 0x00);

    outb(PIC2_COMMAND, 0x11);
    outb(PIC2_DATA, 0x28);
    outb(PIC2_DATA, 0x02);
    outb(PIC2_DATA, 0x01);
    outb(PIC2_DATA, 0x00);

    outb(PIC1_DATA, 0xFC);
    outb(PIC2_DATA, 0xFF);
}

void pic_eoi(uint8_t irq) {
    if (irq >= 8) {
        outb(PIC2_COMMAND, PIC_EOI);
    }
    outb(PIC1_COMMAND, PIC_EOI);
}

void idt_init(void) {
    idtp.limit = (uint16_t)(sizeof(idt) - 1);
    idtp.base = (uint64_t)&idt;

    for (int i = 0; i < 256; i++) {
        idt_set_gate((uint8_t)i, isr0);
    }

    for (int i = 0; i < 48; i++) {
        idt_set_gate((uint8_t)i, isr_table[i]);
    }

    idt_set_gate(LAPIC_TIMER_VECTOR, isr64);
    idt_set_gate(LAPIC_IPI_RESCHED_VECTOR, isr65);

    idt_set_gate_user(0x80, isr128);

    idt_load((uint64_t)&idtp);
}

void idt_reload(void) {
    idt_load((uint64_t)&idtp);
}

void timer_init(void) {
    uint32_t divisor = PIT_FREQUENCY / TICK_FREQUENCY;
    outb(PIT_COMMAND, 0x36);
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFF));
    timer_enabled = 1;
}

int timer_get_ticks(void) {
    return timer_ticks;
}

int timer_is_enabled(void) {
    return timer_enabled;
}

void timer_set_enabled(int enabled) {
    timer_enabled = enabled;
}

void timer_interrupt_handler(void) {
    timer_ticks++;
    timer_switch_count++;
    lwkt_preempt_request();
}

int timer_get_switch_count(void) {
    return timer_switch_count;
}

void timer_reset_stats(void) {
    timer_ticks = 0;
    timer_switch_count = 0;
}

void interrupts_enable(void) {
    __asm__ volatile("sti");
}

void interrupt_handler(uint64_t int_no, uint64_t err_code) {
    (void)err_code;

    if (int_no == LAPIC_TIMER_VECTOR) {
        timer_ticks++;
        timer_switch_count++;
        lwkt_preempt_request();
        lapic_eoi();
        if (!lwkt_in_usersyscall()) {
            lwkt_preempt_check();
        }
        return;
    }

    if (int_no == LAPIC_IPI_RESCHED_VECTOR) {
        lwkt_preempt_request();
        lapic_eoi();
        if (!lwkt_in_usersyscall()) {
            lwkt_preempt_check();
        }
        return;
    }

    if (int_no >= 32 && int_no < 48) {
        uint8_t irq = (uint8_t)(int_no - 32);

        if (irq == IRQ_TIMER) {
            timer_interrupt_handler();
        } else if (irq == IRQ_KEYBOARD) {
            keyboard_irq_handler();
        }

        pic_eoi(irq);
        if (irq == IRQ_TIMER && !lwkt_in_usersyscall()) {
            lwkt_preempt_check();
        }
        return;
    }

    if (int_no < 32) {
        uint64_t cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));

        console_writestring("\nCPU exception ");
        console_write_dec(int_no);
        console_writestring(" err=");
        console_write_hex(err_code);
        console_writestring(" cr2=");
        console_write_hex(cr2);
        console_writestring("\n");
        for (;;) {
            __asm__ volatile("hlt");
        }
    }
}
