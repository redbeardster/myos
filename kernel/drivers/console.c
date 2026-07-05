#include "console.h"
#include "font.h"
#include "spinlock.h"

#define FONT_W 8
#define CHAR_HEIGHT 16

static struct limine_framebuffer *fb;
static volatile uint32_t *pixels;
static uint32_t fg_color = 0xFFFFFF;
static uint32_t bg_color = 0x000000;
static unsigned cursor_col;
static unsigned cursor_row;
static unsigned saved_col;
static unsigned saved_row;
static spinlock_t console_lock;

static const uint32_t palette[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA,
    0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
    0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
};

static uint32_t pack_pixel(uint32_t rgb) {
    if (!fb) {
        return rgb;
    }

    uint32_t r = (rgb >> 16) & 0xFF;
    uint32_t g = (rgb >> 8) & 0xFF;
    uint32_t b = rgb & 0xFF;

    if (fb->bpp == 32) {
        return (r << fb->red_mask_shift) |
               (g << fb->green_mask_shift) |
               (b << fb->blue_mask_shift);
    }

    return rgb;
}

static void putpixel(unsigned x, unsigned y, uint32_t color) {
    if (!pixels || !fb || x >= fb->width || y >= fb->height) {
        return;
    }

    pixels[y * (fb->pitch / 4) + x] = pack_pixel(color);
}

static void draw_char(unsigned col, unsigned row, char c) {
    if (c < 32 || c > 126) {
        c = '?';
    }

    const uint8_t *glyph = font_8x16[(unsigned char)c - 32];
    unsigned px = col * FONT_W;
    unsigned py = row * CHAR_HEIGHT;

    for (unsigned y = 0; y < CHAR_HEIGHT; y++) {
        uint8_t bits = glyph[y];
        for (unsigned x = 0; x < FONT_W; x++) {
            putpixel(px + x, py + y, (bits & (0x80 >> x)) ? fg_color : bg_color);
        }
    }
}

void console_draw_at(unsigned col, unsigned row, char c) {
    if (c < ' ') {
        c = ' ';
    }
    draw_char(col, row, c);
}

void console_write_at(unsigned col, unsigned row, const char *str) {
    unsigned x = col;
    while (str && *str) {
        console_draw_at(x++, row, *str++);
    }
}

void console_write_dec_at(unsigned col, unsigned row, uint64_t n, int width) {
    char buf[24];
    int i = 0;

    if (n == 0) {
        buf[i++] = '0';
    } else {
        while (n > 0) {
            buf[i++] = (char)('0' + (n % 10));
            n /= 10;
        }
    }

    int len = i;
    int pad = width - len;
    if (pad < 0) {
        pad = 0;
    }

    for (int p = 0; p < pad; p++) {
        console_draw_at((unsigned)(col + p), row, ' ');
    }
    for (int j = 0; j < len; j++) {
        console_draw_at((unsigned)(col + pad + j), row, buf[len - 1 - j]);
    }
}

void console_fill_row(unsigned row, unsigned col_start, unsigned col_end, char c) {
    for (unsigned col = col_start; col < col_end; col++) {
        console_draw_at(col, row, c);
    }
}

static void scroll_up(void) {
    if (!fb || !pixels) {
        return;
    }

    unsigned char_h = CHAR_HEIGHT;
    unsigned visible_rows = (unsigned)(fb->height / char_h);
    if (visible_rows == 0) {
        return;
    }

    unsigned copy_rows = visible_rows - 1;
    unsigned row_bytes = (unsigned)fb->pitch * char_h;

    for (unsigned row = 0; row < copy_rows; row++) {
        volatile uint32_t *dst = pixels + row * (fb->pitch / 4) * char_h;
        volatile uint32_t *src = pixels + (row + 1) * (fb->pitch / 4) * char_h;
        for (unsigned i = 0; i < row_bytes / 4; i++) {
            dst[i] = src[i];
        }
    }

    unsigned clear_y = copy_rows * char_h;
    for (unsigned y = clear_y; y < clear_y + char_h; y++) {
        for (unsigned x = 0; x < fb->width; x++) {
            putpixel(x, y, bg_color);
        }
    }

    cursor_row = copy_rows;
    cursor_col = 0;
}

void console_init(struct limine_framebuffer *framebuffer) {
    spin_init(&console_lock);
    fb = framebuffer;
    pixels = (volatile uint32_t *)fb->address;
    cursor_col = 0;
    cursor_row = 0;
    console_setcolor(COLOR_WHITE, COLOR_BLACK);
    console_clear();
}

void console_setcolor(enum console_color fg, enum console_color bg) {
    fg_color = palette[fg & 0xF];
    bg_color = palette[bg & 0xF];
}

void console_set_cursor(unsigned col, unsigned row) {
    cursor_col = col;
    cursor_row = row;
}

void console_save_cursor(void) {
    saved_col = cursor_col;
    saved_row = cursor_row;
}

void console_restore_cursor(void) {
    cursor_col = saved_col;
    cursor_row = saved_row;
}

void console_clear(void) {
    if (!fb || !pixels) {
        return;
    }

    uint32_t bg = pack_pixel(bg_color);
    for (uint64_t y = 0; y < fb->height; y++) {
        for (uint64_t x = 0; x < fb->width; x++) {
            pixels[y * (fb->pitch / 4) + x] = bg;
        }
    }

    cursor_col = 0;
    cursor_row = 0;
}

void console_putchar(char c) {
    spin_lock(&console_lock);
    unsigned max_cols = fb ? (unsigned)(fb->width / FONT_W) : SCREEN_COLS;
    unsigned max_rows = fb ? (unsigned)(fb->height / CHAR_HEIGHT) : SCREEN_ROWS;

    if (c == '\n') {
        cursor_col = 0;
        cursor_row++;
    } else if (c == '\r') {
        cursor_col = 0;
    } else if (c == '\t') {
        cursor_col = (cursor_col + 4) & ~3u;
    } else if (c == '\b') {
        if (cursor_col > 0) {
            cursor_col--;
            draw_char(cursor_col, cursor_row, ' ');
        }
        spin_unlock(&console_lock);
        return;
    } else if (c >= ' ') {
        draw_char(cursor_col, cursor_row, c);
        cursor_col++;
    } else {
        spin_unlock(&console_lock);
        return;
    }

    if (cursor_col >= max_cols) {
        cursor_col = 0;
        cursor_row++;
    }

    if (cursor_row >= max_rows) {
        scroll_up();
    }
    spin_unlock(&console_lock);
}

void console_writestring(const char *str) {
    while (str && *str) {
        console_putchar(*str++);
    }
}

void console_write_dec(uint64_t n) {
    if (n == 0) {
        console_putchar('0');
        return;
    }

    char buf[32];
    int i = 0;
    while (n > 0) {
        buf[i++] = (char)('0' + (n % 10));
        n /= 10;
    }

    while (i > 0) {
        console_putchar(buf[--i]);
    }
}

void console_write_hex(uint64_t n) {
    console_writestring("0x");
    if (n == 0) {
        console_putchar('0');
        return;
    }

    static const char hex[] = "0123456789ABCDEF";
    char buf[16];
    int i = 0;
    while (n > 0) {
        buf[i++] = hex[n & 0xF];
        n >>= 4;
    }

    while (i > 0) {
        console_putchar(buf[--i]);
    }
}
