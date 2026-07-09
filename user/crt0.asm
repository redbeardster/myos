BITS 64

global _start
extern main

%define MYOS_SYS_EXIT 0

section .text
_start:
    ; rdi = exec arg0 packed by kernel (0 if unset)
    xor rsi, rsi
    call main

    mov rax, MYOS_SYS_EXIT
    xor rdi, rdi
    int 0x80

.hang:
    hlt
    jmp .hang
