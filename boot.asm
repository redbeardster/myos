; boot.asm
section .multiboot
    align 4
    dd 0x1BADB002
    dd 0x00
    dd -(0x1BADB002)

section .text
global start
extern kmain

start:
    mov esp, stack_top
    push ebx
    push eax
    call kmain
    cli
    hlt

section .bss
    align 16
    resb 16384
stack_top:
