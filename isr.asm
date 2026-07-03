; isr.asm - Обработчики прерываний
bits 32

section .text
global isr0, isr1, isr2, isr3, isr4, isr5, isr6, isr7
global isr8, isr9, isr10, isr11, isr12, isr13, isr14, isr15
global isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23
global isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31
global idt_load, gdt_load

; Внешняя функция C
extern interrupt_handler

; Внешние переменные из C
extern idtp

; Макрос для создания обработчиков без кода ошибки
%macro ISR_NOERR 1
  isr%1:
    push 0          ; Фиктивный код ошибки
    push %1         ; Номер прерывания
    jmp isr_common
%endmacro

; Макрос для создания обработчиков с кодом ошибки
%macro ISR_ERR 1
  isr%1:
    push %1         ; Номер прерывания
    jmp isr_common
%endmacro

; Общий обработчик
isr_common:
    pusha           ; Сохраняем все регистры

    ; Вызываем C-обработчик
    push esp        ; Указатель на стек
    call interrupt_handler
    add esp, 4

    popa            ; Восстанавливаем регистры
    add esp, 8      ; Убираем номер прерывания и код ошибки
    iret            ; Возврат из прерывания

; Создаём обработчики для всех 32 прерываний
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

; Загрузка IDT
idt_load:
    lidt [idtp]
    ret

; Загрузка GDT (принимает указатель на gdt_ptr в стеке - стандартный вызов C)
gdt_load:
    ; Получаем аргумент (указатель на gdt_ptr) из стека
    mov eax, [esp + 4]   ; Первый аргумент функции
    mov [gdt_ptr_temp], eax  ; Сохраняем указатель

    ; Загружаем GDT
    lgdt [gdt_ptr_temp]

    ; Перезагружаем сегменты
    jmp 0x08:.gdt_reload
.gdt_reload:
    mov ax, 0x10    ; Сегмент данных ядра
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    ret

section .data
gdt_ptr_temp:
    dw 0
    dd 0
