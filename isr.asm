bits 64

section .text

extern interrupt_handler
extern lwkt_thread_exit

global isr0, isr1, isr2, isr3, isr4, isr5, isr6, isr7
global isr8, isr9, isr10, isr11, isr12, isr13, isr14, isr15
global isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23
global isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31
global isr32, isr33, isr34, isr35, isr36, isr37, isr38, isr39
global isr40, isr41, isr42, isr43, isr44, isr45, isr46, isr47
global gdt_load, idt_load, switch_context, thread_bootstrap
global isr128, user_enter_asm, load_tss

%macro ISR_NOERR 1
isr%1:
    push qword 0
    push %1
    jmp isr_common
%endmacro

%macro ISR_ERR 1
isr%1:
    push %1
    jmp isr_common
%endmacro

isr_common:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov rdi, [rsp + 15 * 8]
    mov rsi, [rsp + 15 * 8 + 8]
    call interrupt_handler

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    add rsp, 16
    iretq

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_NOERR 17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_NOERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_NOERR 30
ISR_NOERR 31
ISR_NOERR 32
ISR_NOERR 33
ISR_NOERR 34
ISR_NOERR 35
ISR_NOERR 36
ISR_NOERR 37
ISR_NOERR 38
ISR_NOERR 39
ISR_NOERR 40
ISR_NOERR 41
ISR_NOERR 42
ISR_NOERR 43
ISR_NOERR 44
ISR_NOERR 45
ISR_NOERR 46
ISR_NOERR 47

; int 0x80 syscall vector
isr128:
    push qword 0
    push 128
    jmp syscall_common

syscall_common:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov rdi, [rsp + 14 * 8]     ; syscall number (rax)
    mov rsi, [rsp + 9 * 8]      ; arg1 (rdi)
    mov rdx, [rsp + 10 * 8]     ; arg2 (rsi)
    mov rcx, [rsp + 11 * 8]     ; arg3 (rdx)
    call syscall_dispatch
    mov [rsp + 14 * 8], rax

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    add rsp, 16
    iretq

extern syscall_dispatch

global user_enter_asm

user_enter_asm:
    ; rdi = rip, rsi = rsp, rdx = &saved_kernel_rsp
    mov [rdx], rsp
    push qword 0x23
    push rsi
    pushfq
    pop rax
    or rax, 0x202
    push rax
    push qword 0x1B
    push rdi
    iretq

global load_tss
load_tss:
    ltr di
    ret

global gdt_load
gdt_load:
    lgdt [rdi]
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax
    pop rax             ; return address (RIP)
    push qword 0x08     ; new CS selector (64-bit kernel code)
    push rax            ; RIP
    retfq               ; far return -> reloads CS from our GDT

global idt_load
idt_load:
    lidt [rdi]
    ret

global switch_context
switch_context:
    ; rdi = &old->rsp, rsi = new rsp
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
    mov [rdi], rsp
    mov rsp, rsi
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    ret


global thread_bootstrap
thread_bootstrap:
    pop rdi
    pop rax
    call rax
    jmp lwkt_thread_exit
