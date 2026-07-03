; boot.asm
bits 32
section .multiboot
    ; Магическое число Multiboot
    dd 0x1BADB002
    ; Флаги (0 = ничего особенного не нужно)
    dd 0x0
    ; Контрольная сумма (-магическое число - флаги)
    dd -(0x1BADB002 + 0x0)

section .text
global start
extern kmain          ; Функция из kernel.c

start:
    cli               ; Отключаем прерывания
    mov esp, stack_space ; Устанавливаем указатель стека
    push eax          ; Сохраняем магическое число Multiboot (для совместимости)
    push ebx          ; Сохраняем указатель на структуру Multiboot
    call kmain        ; Вызываем главную функцию ядра
    hlt               ; Останавливаем процессор (если kmain вернет управление)

section .bss
    resb 8192         ; 8 КБ для стека
stack_space:
