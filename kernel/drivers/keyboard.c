#include "keyboard.h"
#include "io.h"
#include "lwkt.h"
#include "spinlock.h"

#define KEYBOARD_PORT 0x60
#define KBD_RING_SIZE 64

static volatile uint8_t kbd_ring[KBD_RING_SIZE];
static volatile int kbd_head;
static volatile int kbd_tail;

static struct lwkt_thread *kbd_reader;
static spinlock_t kbd_lock;

static const char scancode_to_ascii[] = {
    0,   0,   '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 0,
    '\b', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0,   'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0,
    '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' '
};

static const char scancode_to_ascii_shift[] = {
    0,   0,   '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', 0,
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0,   'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0,
    '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' '
};

static int shift_pressed;
static int caps_lock;

static void ring_push(uint8_t scancode) {
    int next = (kbd_head + 1) % KBD_RING_SIZE;
    if (next == kbd_tail) {
        return;
    }
    kbd_ring[kbd_head] = scancode;
    kbd_head = next;
}

static int ring_pop(uint8_t *scancode) {
    if (kbd_head == kbd_tail) {
        return 0;
    }
    *scancode = kbd_ring[kbd_tail];
    kbd_tail = (kbd_tail + 1) % KBD_RING_SIZE;
    return 1;
}

void keyboard_init(void) {
    kbd_head = 0;
    kbd_tail = 0;
    shift_pressed = 0;
    caps_lock = 0;
    kbd_reader = NULL;
    spin_init(&kbd_lock);
}

void keyboard_set_reader(struct lwkt_thread *t) {
    uint64_t irqf;
    spin_lock_irqsave(&kbd_lock, &irqf);
    kbd_reader = t;
    spin_unlock_irqrestore(&kbd_lock, irqf);
}

void keyboard_clear_reader(struct lwkt_thread *t) {
    uint64_t irqf;
    spin_lock_irqsave(&kbd_lock, &irqf);
    if (kbd_reader == t) {
        kbd_reader = NULL;
    }
    spin_unlock_irqrestore(&kbd_lock, irqf);
}

void keyboard_irq_handler(void) {
    ring_push(inb(KEYBOARD_PORT));

    uint64_t irqf;
    spin_lock_irqsave(&kbd_lock, &irqf);
    struct lwkt_thread *t = kbd_reader;
    kbd_reader = NULL;
    spin_unlock_irqrestore(&kbd_lock, irqf);

    if (t) {
        lwkt_unblock(t);
    }
}

int keyboard_has_scancode(void) {
    return kbd_head != kbd_tail;
}

uint8_t keyboard_read_scancode(void) {
    uint8_t scancode = 0;
    while (!ring_pop(&scancode)) {
        __asm__ volatile("hlt");
    }
    return scancode;
}

static char translate_scancode(uint8_t scancode) {
    if (scancode == 0x2A || scancode == 0x36) {
        shift_pressed = 1;
        return 0;
    }
    if (scancode == 0xAA || scancode == 0xB6) {
        shift_pressed = 0;
        return 0;
    }
    if (scancode == 0x3A) {
        caps_lock = !caps_lock;
        return 0;
    }

    if (scancode >= 0x80) {
        return 0;
    }

    char c;
    if (shift_pressed || caps_lock) {
        c = scancode_to_ascii_shift[scancode];
    } else {
        c = scancode_to_ascii[scancode];
    }
    return c;
}

char keyboard_poll_char(void) {
    uint8_t scancode;

    while (ring_pop(&scancode)) {
        char c = translate_scancode(scancode);
        if (c != 0) {
            return c;
        }
    }
    return 0;
}

char keyboard_read_char(void) {
    for (;;) {
        char c = keyboard_poll_char();
        if (c != 0) {
            return c;
        }
        __asm__ volatile("hlt");
    }
}
