#include "keyboard.h"

#include "io.h"
#include "kbdd.h"
#include "spinlock.h"

#define KEYBOARD_PORT 0x60
#define KEYBOARD_STATUS_PORT 0x64
#define KBD_RING_SIZE 64

static volatile uint8_t kbd_ring[KBD_RING_SIZE];
static volatile int kbd_head;
static volatile int kbd_tail;

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

static void ring_push_locked(uint8_t scancode) {
    int next = (kbd_head + 1) % KBD_RING_SIZE;
    if (next == kbd_tail) {
        return;
    }
    kbd_ring[kbd_head] = scancode;
    kbd_head = next;
}

static int ring_pop_locked(uint8_t *scancode) {
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
    spin_init(&kbd_lock);
}

void keyboard_irq_handler(void) {
    uint8_t scancode = inb(KEYBOARD_PORT);

    uint64_t irqf;
    spin_lock_irqsave(&kbd_lock, &irqf);
    ring_push_locked(scancode);
    spin_unlock_irqrestore(&kbd_lock, irqf);

    kbdd_irq_notify();
}

int keyboard_scancode_pending(void) {
    uint64_t irqf;
    spin_lock_irqsave(&kbd_lock, &irqf);
    int has = kbd_head != kbd_tail;
    spin_unlock_irqrestore(&kbd_lock, irqf);
    return has;
}

int keyboard_pop_scancode(uint8_t *scancode) {
    if (!scancode) {
        return 0;
    }

    uint64_t irqf;
    spin_lock_irqsave(&kbd_lock, &irqf);
    int ok = ring_pop_locked(scancode);
    spin_unlock_irqrestore(&kbd_lock, irqf);
    return ok;
}

int keyboard_poll_scancode(uint8_t *scancode) {
    if (!scancode) {
        return 0;
    }
    uint8_t status = inb(KEYBOARD_STATUS_PORT);
    if ((status & 0x01) == 0) {
        return 0;
    }
    *scancode = inb(KEYBOARD_PORT);
    return 1;
}

char keyboard_translate_scancode(uint8_t scancode) {
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

    if (shift_pressed || caps_lock) {
        return scancode_to_ascii_shift[scancode];
    }
    return scancode_to_ascii[scancode];
}
