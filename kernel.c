// kernel.c - Ядро с командной строкой
// ============================================================================
// Базовые типы и определения
// ============================================================================

#define VIDEO_MEMORY 0xB8000
#define SCREEN_WIDTH 80
#define SCREEN_HEIGHT 25
#define PAGE_SIZE 4096
#define MEMORY_START 0x100000   // 1MB
#define MEMORY_END 0x2000000    // 32MB
#define MAX_COMMAND_LEN 256
#define HISTORY_SIZE 10

typedef unsigned int uint32_t;
typedef unsigned short uint16_t;
typedef unsigned char uint8_t;
typedef int int32_t;

#define NULL ((void*)0)

// ============================================================================
// Цвета VGA
// ============================================================================

enum vga_color {
    COLOR_BLACK = 0,
    COLOR_BLUE = 1,
    COLOR_GREEN = 2,
    COLOR_CYAN = 3,
    COLOR_RED = 4,
    COLOR_MAGENTA = 5,
    COLOR_BROWN = 6,
    COLOR_LIGHT_GREY = 7,
    COLOR_DARK_GREY = 8,
    COLOR_LIGHT_BLUE = 9,
    COLOR_LIGHT_GREEN = 10,
    COLOR_LIGHT_CYAN = 11,
    COLOR_LIGHT_RED = 12,
    COLOR_LIGHT_MAGENTA = 13,
    COLOR_LIGHT_BROWN = 14,
    COLOR_WHITE = 15,
};

// ============================================================================
// Работа с портами ввода-вывода
// ============================================================================

static inline unsigned char inb(unsigned short port) {
    unsigned char result;
    __asm__ volatile("inb %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}

static inline void outb(unsigned short port, unsigned char data) {
    __asm__ volatile("outb %0, %1" : : "a"(data), "Nd"(port));
}

// ============================================================================
// Функции работы со строками (своя реализация)
// ============================================================================

int strlen(const char* str) {
    int len = 0;
    while (str[len]) len++;
    return len;
}

int strcmp(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(unsigned char*)s1 - *(unsigned char*)s2;
}

int strncmp(const char* s1, const char* s2, int n) {
    for (int i = 0; i < n; i++) {
        if (s1[i] != s2[i] || s1[i] == '\0' || s2[i] == '\0') {
            return s1[i] - s2[i];
        }
    }
    return 0;
}

void strcpy(char* dest, const char* src) {
    while (*src) {
        *dest++ = *src++;
    }
    *dest = '\0';
}

void strcat(char* dest, const char* src) {
    while (*dest) dest++;
    while (*src) {
        *dest++ = *src++;
    }
    *dest = '\0';
}

// ============================================================================
// Терминал (вывод на экран)
// ============================================================================

static uint16_t* const vga_buffer = (uint16_t*)VIDEO_MEMORY;
static uint32_t vga_index = 0;
static uint8_t terminal_color = COLOR_WHITE | (COLOR_BLACK << 4);

static void terminal_putchar(char c) {
    if (c == '\n') {
        vga_index = (vga_index / SCREEN_WIDTH + 1) * SCREEN_WIDTH;
        return;
    }

    if (c == '\r') {
        vga_index = (vga_index / SCREEN_WIDTH) * SCREEN_WIDTH;
        return;
    }

    if (c == '\t') {
        for (int i = 0; i < 4; i++) {
            terminal_putchar(' ');
        }
        return;
    }

    if (c == '\b') {
        if (vga_index > 0) {
            vga_index--;
            vga_buffer[vga_index] = (uint16_t)' ' | ((uint16_t)terminal_color << 8);
        }
        return;
    }

    if (vga_index >= SCREEN_WIDTH * SCREEN_HEIGHT) {
        for (int i = 0; i < (SCREEN_HEIGHT - 1) * SCREEN_WIDTH; i++) {
            vga_buffer[i] = vga_buffer[i + SCREEN_WIDTH];
        }
        for (int i = (SCREEN_HEIGHT - 1) * SCREEN_WIDTH; i < SCREEN_WIDTH * SCREEN_HEIGHT; i++) {
            vga_buffer[i] = (uint16_t)' ' | ((uint16_t)terminal_color << 8);
        }
        vga_index = (SCREEN_HEIGHT - 1) * SCREEN_WIDTH;
    }

    vga_buffer[vga_index++] = (uint16_t)c | ((uint16_t)terminal_color << 8);
}

void terminal_writestring(const char* str) {
    while (*str) {
        terminal_putchar(*str++);
    }
}

void terminal_write_dec(uint32_t n) {
    if (n == 0) {
        terminal_putchar('0');
        return;
    }

    char buffer[32];
    int i = 0;
    while (n > 0) {
        buffer[i++] = '0' + (n % 10);
        n /= 10;
    }
    while (i > 0) {
        terminal_putchar(buffer[--i]);
    }
}

void terminal_write_hex(uint32_t n) {
    terminal_writestring("0x");
    if (n == 0) {
        terminal_putchar('0');
        return;
    }

    char hex[] = "0123456789ABCDEF";
    char buffer[8];
    int i = 0;
    while (n > 0) {
        buffer[i++] = hex[n & 0xF];
        n >>= 4;
    }
    while (i > 0) {
        terminal_putchar(buffer[--i]);
    }
}

void terminal_clear(void) {
    uint16_t blank = (uint16_t)' ' | ((uint16_t)terminal_color << 8);
    for (int i = 0; i < SCREEN_WIDTH * SCREEN_HEIGHT; i++) {
        vga_buffer[i] = blank;
    }
    vga_index = 0;
}

void terminal_setcolor(uint8_t color) {
    terminal_color = color;
}

// ============================================================================
// Менеджер памяти (битовая карта)
// ============================================================================

#define TOTAL_PAGES ((MEMORY_END - MEMORY_START) / PAGE_SIZE)
static uint8_t page_bitmap[(TOTAL_PAGES / 8) + 1];

void memory_init(void) {
    terminal_writestring("Initializing memory manager...\n");

    for (int i = 0; i < sizeof(page_bitmap); i++) {
        page_bitmap[i] = 0xFF;
    }

    for (uint32_t i = 0; i < TOTAL_PAGES; i++) {
        page_bitmap[i / 8] &= ~(1 << (i % 8));
    }

    terminal_writestring("Total pages: ");
    terminal_write_dec(TOTAL_PAGES);
    terminal_writestring(" (");
    terminal_write_dec(TOTAL_PAGES * PAGE_SIZE / 1024);
    terminal_writestring(" KB)\n");
}

void* alloc_page(void) {
    for (uint32_t i = 0; i < TOTAL_PAGES; i++) {
        if (!(page_bitmap[i / 8] & (1 << (i % 8)))) {
            page_bitmap[i / 8] |= (1 << (i % 8));
            return (void*)(MEMORY_START + i * PAGE_SIZE);
        }
    }
    terminal_writestring("ERROR: Out of memory!\n");
    return NULL;
}

void* alloc_pages(uint32_t count) {
    if (count == 0) return NULL;

    for (uint32_t start = 0; start <= TOTAL_PAGES - count; start++) {
        int found = 1;
        for (uint32_t i = 0; i < count; i++) {
            if (page_bitmap[(start + i) / 8] & (1 << ((start + i) % 8))) {
                found = 0;
                break;
            }
        }

        if (found) {
            for (uint32_t i = 0; i < count; i++) {
                page_bitmap[(start + i) / 8] |= (1 << ((start + i) % 8));
            }
            return (void*)(MEMORY_START + start * PAGE_SIZE);
        }
    }

    terminal_writestring("ERROR: Not enough contiguous memory!\n");
    return NULL;
}

void free_page(void* addr) {
    uint32_t addr_int = (uint32_t)addr;
    if (addr_int < MEMORY_START || addr_int >= MEMORY_END) {
        terminal_writestring("ERROR: Invalid page address!\n");
        return;
    }
    uint32_t page_num = (addr_int - MEMORY_START) / PAGE_SIZE;
    if (page_num < TOTAL_PAGES) {
        page_bitmap[page_num / 8] &= ~(1 << (page_num % 8));
    }
}

void print_memory_info(void) {
    uint32_t free_count = 0;
    for (uint32_t i = 0; i < TOTAL_PAGES; i++) {
        if (!(page_bitmap[i / 8] & (1 << (i % 8)))) {
            free_count++;
        }
    }
    uint32_t used_count = TOTAL_PAGES - free_count;

    terminal_writestring("\n=== Memory Info ===\n");
    terminal_writestring("Total pages: ");
    terminal_write_dec(TOTAL_PAGES);
    terminal_writestring("\n");
    terminal_writestring("Used pages: ");
    terminal_write_dec(used_count);
    terminal_writestring("\n");
    terminal_writestring("Free pages: ");
    terminal_write_dec(free_count);
    terminal_writestring("\n");
    terminal_writestring("Total memory: ");
    terminal_write_dec(TOTAL_PAGES * PAGE_SIZE / 1024);
    terminal_writestring(" KB\n");
    terminal_writestring("Used memory: ");
    terminal_write_dec(used_count * PAGE_SIZE / 1024);
    terminal_writestring(" KB\n");
    terminal_writestring("Free memory: ");
    terminal_write_dec(free_count * PAGE_SIZE / 1024);
    terminal_writestring(" KB\n");
    terminal_writestring("==================\n");
}

// ============================================================================
// Клавиатура
// ============================================================================

#define KEYBOARD_PORT 0x60
#define KEYBOARD_STATUS_PORT 0x64

static const char scancode_to_ascii[] = {
    0,   0,   '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 0,
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0,   'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0,
    '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' '
};

static const char scancode_to_ascii_shift[] = {
    0,   0,   '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', 0,
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0,   'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0,
    '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' '
};

static int shift_pressed = 0;
static int caps_lock = 0;

static int keyboard_has_data(void) {
    return (inb(KEYBOARD_STATUS_PORT) & 1) != 0;
}

char keyboard_read_char(void) {
    while (1) {
        if (keyboard_has_data()) {
            unsigned char scancode = inb(KEYBOARD_PORT);

            if (scancode == 0x2A || scancode == 0x36) {
                shift_pressed = 1;
                continue;
            }
            if (scancode == 0xAA || scancode == 0xB6) {
                shift_pressed = 0;
                continue;
            }
            if (scancode == 0x3A) {
                caps_lock = !caps_lock;
                continue;
            }

            if (scancode < 0x80) {
                char c;
                if (shift_pressed || caps_lock) {
                    c = scancode_to_ascii_shift[scancode];
                } else {
                    c = scancode_to_ascii[scancode];
                }
                if (c != 0) {
                    return c;
                }
            }
        }
    }
}

// ============================================================================
// КОМАНДНАЯ СТРОКА (ПЕРЕМЕННЫЕ)
// ============================================================================

static char command_buffer[MAX_COMMAND_LEN];
static int command_pos = 0;

// ============================================================================
// ИСТОРИЯ КОМАНД
// ============================================================================

static char command_history[HISTORY_SIZE][MAX_COMMAND_LEN];
static int history_count = 0;
static int history_pos = -1;

// Добавить команду в историю
void add_to_history(const char* cmd) {
    if (cmd[0] == '\0') return;

    if (history_count > 0 && strcmp(command_history[history_count - 1], cmd) == 0) {
        return;
    }

    if (history_count < HISTORY_SIZE) {
        strcpy(command_history[history_count++], cmd);
    } else {
        for (int i = 0; i < HISTORY_SIZE - 1; i++) {
            strcpy(command_history[i], command_history[i + 1]);
        }
        strcpy(command_history[HISTORY_SIZE - 1], cmd);
    }
}

// Получить предыдущую команду (стрелка вверх)
void get_previous_command(void) {
    if (history_count == 0) return;

    if (history_pos == -1) {
        history_pos = history_count - 1;
    } else if (history_pos > 0) {
        history_pos--;
    }

    while (command_pos > 0) {
        terminal_putchar('\b');
        command_pos--;
    }

    strcpy(command_buffer, command_history[history_pos]);
    command_pos = strlen(command_buffer);
    terminal_writestring(command_buffer);
}

// Получить следующую команду (стрелка вниз)
void get_next_command(void) {
    if (history_count == 0 || history_pos == -1) return;

    if (history_pos < history_count - 1) {
        history_pos++;
        while (command_pos > 0) {
            terminal_putchar('\b');
            command_pos--;
        }
        strcpy(command_buffer, command_history[history_pos]);
        command_pos = strlen(command_buffer);
        terminal_writestring(command_buffer);
    } else {
        history_pos = -1;
        while (command_pos > 0) {
            terminal_putchar('\b');
            command_pos--;
        }
        command_buffer[0] = '\0';
        command_pos = 0;
    }
}

// ============================================================================
// КОМАНДНАЯ СТРОКА (ФУНКЦИИ)
// ============================================================================

void print_prompt(void) {
    terminal_writestring("\nMyOS> ");
}

// Обработка команд
void process_command(const char* cmd) {
    if (cmd[0] == '\0') {
        return;
    }

    // Команда: help
    if (strcmp(cmd, "help") == 0) {
        terminal_writestring("\nAvailable commands:\n");
        terminal_writestring("  help     - Show this help message\n");
        terminal_writestring("  clear    - Clear the screen\n");
        terminal_writestring("  meminfo  - Show memory information\n");
        terminal_writestring("  reboot   - Reboot the system\n");
        terminal_writestring("  echo     - Echo text back\n");
        terminal_writestring("  about    - Show system info\n");
        terminal_writestring("  alloc N  - Allocate N pages\n");
        terminal_writestring("  free ADDR - Free page at address\n");
        return;
    }

    // Команда: clear
    if (strcmp(cmd, "clear") == 0) {
        terminal_clear();
        return;
    }

    // Команда: meminfo
    if (strcmp(cmd, "meminfo") == 0) {
        print_memory_info();
        return;
    }

    // Команда: reboot
    if (strcmp(cmd, "reboot") == 0) {
        terminal_writestring("\nRebooting...\n");
        outb(0x64, 0xFE);
        while (1);
    }

    // Команда: about
    if (strcmp(cmd, "about") == 0) {
        terminal_writestring("\nMyOS Kernel v1.0\n");
        terminal_writestring("Built with: GCC + NASM\n");
        terminal_writestring("Running in QEMU\n");
        terminal_writestring("Features: Terminal, Memory Manager, Command Line, History\n");
        return;
    }

    // Команда: echo
    if (strncmp(cmd, "echo ", 5) == 0) {
        terminal_writestring("\n");
        terminal_writestring(cmd + 5);
        return;
    }

    // Команда: alloc
    if (strncmp(cmd, "alloc ", 6) == 0) {
        const char* num_str = cmd + 6;
        int count = 0;
        while (*num_str >= '0' && *num_str <= '9') {
            count = count * 10 + (*num_str - '0');
            num_str++;
        }

        if (count == 0) count = 1;

        if (count > 100) {
            terminal_writestring("\nError: Too many pages (max 100)");
            return;
        }

        void* ptr = alloc_pages(count);
        if (ptr != NULL) {
            terminal_writestring("\nAllocated ");
            terminal_write_dec(count);
            terminal_writestring(" page(s) at ");
            terminal_write_hex((uint32_t)ptr);
        }
        return;
    }

    // Команда: free
    if (strncmp(cmd, "free ", 5) == 0) {
        const char* addr_str = cmd + 5;
        uint32_t addr = 0;

        if (addr_str[0] == '0' && (addr_str[1] == 'x' || addr_str[1] == 'X')) {
            addr_str += 2;
        }

        while (*addr_str) {
            char c = *addr_str++;
            if (c >= '0' && c <= '9') {
                addr = addr * 16 + (c - '0');
            } else if (c >= 'A' && c <= 'F') {
                addr = addr * 16 + (c - 'A' + 10);
            } else if (c >= 'a' && c <= 'f') {
                addr = addr * 16 + (c - 'a' + 10);
            } else {
                break;
            }
        }

        if (addr == 0) {
            terminal_writestring("\nError: Invalid address");
            return;
        }

        free_page((void*)addr);
        terminal_writestring("\nFreed page at ");
        terminal_write_hex(addr);
        return;
    }

    // Неизвестная команда
    terminal_writestring("\nUnknown command: ");
    terminal_writestring(cmd);
    terminal_writestring("\nType 'help' for available commands.");
}

// Обработка ввода команд
void handle_command_input(void) {
    char c = keyboard_read_char();

    if (c == '\n') {
        terminal_putchar(c);
        command_buffer[command_pos] = '\0';
        if (command_pos > 0) {
            add_to_history(command_buffer);
            process_command(command_buffer);
        }
        command_pos = 0;
        command_buffer[0] = '\0';
        history_pos = -1;
        print_prompt();
        return;
    }

    if (c == '\b') {
        if (command_pos > 0) {
            command_pos--;
            terminal_putchar(c);
        }
        return;
    }

    // Обработка стрелок
    if (c == 0x1B) {
        char c2 = keyboard_read_char();
        if (c2 == '[') {
            char c3 = keyboard_read_char();
            if (c3 == 'A') {
                get_previous_command();
                return;
            } else if (c3 == 'B') {
                get_next_command();
                return;
            }
        }
        return;
    }

    // Обычный символ
    if (c >= ' ' && c <= '~') {
        if (command_pos < MAX_COMMAND_LEN - 1) {
            if (history_pos != -1) {
                history_pos = -1;
            }
            command_buffer[command_pos++] = c;
            terminal_putchar(c);
        }
    }
}

// ============================================================================
// Главная функция ядра
// ============================================================================

void kmain(unsigned int magic, unsigned int addr) {
    terminal_clear();
    terminal_setcolor(COLOR_WHITE | (COLOR_BLUE << 4));

    terminal_writestring("╔══════════════════════════════════════════════════════╗\n");
    terminal_writestring("║              MyOS Kernel v1.0                      ║\n");
    terminal_writestring("╚══════════════════════════════════════════════════════╝\n\n");

    terminal_writestring("System initialized successfully!\n");
    terminal_writestring("Type 'help' for available commands.\n");

    memory_init();

    print_prompt();

    while (1) {
        handle_command_input();
    }
}
