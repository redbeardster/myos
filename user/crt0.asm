BITS 64

global _start
extern main

%define MYOS_SYS_EXIT 0

section .text
_start:
    xor rdi, rdi
    xor rsi, rsi
    call main

    mov rax, MYOS_SYS_EXIT
    xor rdi, rdi
    int 0x80

.hang:
    hlt
    jmp .hang
