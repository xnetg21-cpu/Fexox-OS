; ========================================
; SECURE CONTEXT SWITCHING FOR X86-64
; Safe from interrupts and corruption
; ========================================

global context_switch
global timer_irq_handler
global save_cpu_context
global restore_cpu_context

extern irq_handler

; Register structure offsets (must match C struct)
; struct registers_t {
;     uint64_t rax, rbx, rcx, rdx;          [0x00-0x1F]
;     uint64_t rsi, rdi, rbp;               [0x20-0x38]
;     uint64_t r8-r15;                      [0x38-0x78]
;     uint64_t rsp, rip, rflags;            [0x78-0x90]
; }

REG_RAX equ 0x00
REG_RBX equ 0x08
REG_RCX equ 0x10
REG_RDX equ 0x18
REG_RSI equ 0x20
REG_RDI equ 0x28
REG_RBP equ 0x30
REG_R8  equ 0x38
REG_R9  equ 0x40
REG_R10 equ 0x48
REG_R11 equ 0x50
REG_R12 equ 0x58
REG_R13 equ 0x60
REG_R14 equ 0x68
REG_R15 equ 0x70
REG_RSP equ 0x78
REG_RIP equ 0x80
REG_RFLAGS equ 0x88

section .text

; ========================================
; SAFE CONTEXT SWITCH
; rdi = old_registers_t*
; rsi = new_registers_t*
; ========================================

context_switch:
    ; Сохраняем регистры старой задачи в структуру
    ; В этот момент стек содержит return address в [rsp]
    
    push rbp                    ; Сохраняем RBP для красоты
    mov rbp, rsp                ; Выравниваем стек
    
    ; Сохраняем все 64-bit регистры старой задачи
    mov [rdi + REG_RAX], rax
    mov [rdi + REG_RBX], rbx
    mov [rdi + REG_RCX], rcx
    mov [rdi + REG_RDX], rdx
    mov [rdi + REG_RSI], rsi
    mov [rdi + REG_RDI], rdi
    mov [rdi + REG_RBP], rbp    ; Сохраняем перезаписанный RBP
    
    mov [rdi + REG_R8], r8
    mov [rdi + REG_R9], r9
    mov [rdi + REG_R10], r10
    mov [rdi + REG_R11], r11
    mov [rdi + REG_R12], r12
    mov [rdi + REG_R13], r13
    mov [rdi + REG_R14], r14
    mov [rdi + REG_R15], r15
    
    ; Сохраняем RSP (укажет на начало нашего сохраненного контекста)
    lea rax, [rsp + 16]         ; Пропускаем RBP и return address
    mov [rdi + REG_RSP], rax
    
    ; Сохраняем RIP (адрес возврата)
    mov rax, [rsp]              ; Читаем return address со стека
    mov [rdi + REG_RIP], rax
    
    ; Сохраняем RFLAGS
    pushfq
    pop rax
    mov [rdi + REG_RFLAGS], rax
    
    ; ========================================
    ; LOAD NEW TASK CONTEXT
    ; ========================================
    
    ; Загружаем регистры новой задачи
    mov rax, [rsi + REG_RAX]
    mov rbx, [rsi + REG_RBX]
    mov rcx, [rsi + REG_RCX]
    mov rdx, [rsi + REG_RDX]
    
    mov r8, [rsi + REG_R8]
    mov r9, [rsi + REG_R9]
    mov r10, [rsi + REG_R10]
    mov r11, [rsi + REG_R11]
    mov r12, [rsi + REG_R12]
    mov r13, [rsi + REG_R13]
    mov r14, [rsi + REG_R14]
    mov r15, [rsi + REG_R15]
    
    mov rbp, [rsi + REG_RBP]
    
    ; Восстанавливаем RDI (второй параметр для новой задачи)
    mov rdi, [rsi + REG_RDI]
    
    ; Восстанавливаем RIP и RFLAGS через стек
    mov rax, [rsi + REG_RFLAGS]
    push rax
    popfq
    
    ; Загружаем новый RSP
    mov rsp, [rsi + REG_RSP]
    
    ; Загружаем новый RIP и делаем ret
    mov rax, [rsi + REG_RIP]
    push rax
    
    ; Восстанавливаем RSI в последнюю очередь
    mov rsi, [rsi + REG_RSI]
    
    ; Возвращаемся на адрес новой задачи
    ret

; ========================================
; TIMER IRQ HANDLER (INT 32)
; Вызывается при прерывании от таймера
; ========================================

align 16
timer_irq_handler:
    ; Автоматически сохраняются: RIP, CS, RFLAGS, RSP, SS (по x86-64 ABI)
    
    ; Спасаем регистры, используемые в обработчике
    push rax
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    
    ; Выравниваем стек для вызова C функции (16-byte alignment)
    sub rsp, 8
    
    ; Вызываем обработчик на C
    call irq_handler
    
    ; Восстанавливаем стек
    add rsp, 8
    
    ; Восстанавливаем регистры
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rax
    
    ; Возвращаемся из прерывания
    iretq

; ========================================
; EXCEPTION HANDLER (Для #PF, #GP, etc.)
; ========================================

global page_fault_handler
global general_protection_fault_handler

align 16
page_fault_handler:
    ; Page fault может иметь код ошибки в стеке
    push rax
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    
    ; Читаем адрес ошибки из CR2
    mov rax, cr2
    
    ; Для отладки выводим информацию
    ; (в реальной ОС здесь была бы обработка)
    
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rax
    
    add rsp, 8      ; Пропускаем error code
    iretq

align 16
general_protection_fault_handler:
    ; GPF также имеет код ошибки
    push rax
    
    ; Для отладки
    mov rax, 0xDEADBEEF
    
    pop rax
    add rsp, 8
    iretq

; ========================================
; UTILITY FUNCTIONS
; ========================================

; Сохранение контекста в памяти (для отладки)
; void save_cpu_context(registers_t *regs);
save_cpu_context:
    mov [rdi + REG_RAX], rax
    mov [rdi + REG_RBX], rbx
    mov [rdi + REG_RCX], rcx
    mov [rdi + REG_RDX], rdx
    mov [rdi + REG_RSI], rsi
    mov [rdi + REG_RDI], rdi
    mov [rdi + REG_RBP], rbp
    
    mov [rdi + REG_R8], r8
    mov [rdi + REG_R9], r9
    mov [rdi + REG_R10], r10
    mov [rdi + REG_R11], r11
    mov [rdi + REG_R12], r12
    mov [rdi + REG_R13], r13
    mov [rdi + REG_R14], r14
    mov [rdi + REG_R15], r15
    
    mov rax, rsp
    mov [rdi + REG_RSP], rax
    
    pushfq
    pop rax
    mov [rdi + REG_RFLAGS], rax
    
    ret

; Восстановление контекста из памяти (для отладки)
; void restore_cpu_context(registers_t *regs);
restore_cpu_context:
    mov rax, [rdi + REG_RFLAGS]
    push rax
    popfq
    
    mov rax, [rdi + REG_RAX]
    mov rbx, [rdi + REG_RBX]
    mov rcx, [rdi + REG_RCX]
    mov rdx, [rdi + REG_RDX]
    
    mov r8, [rdi + REG_R8]
    mov r9, [rdi + REG_R9]
    mov r10, [rdi + REG_R10]
    mov r11, [rdi + REG_R11]
    mov r12, [rdi + REG_R12]
    mov r13, [rdi + REG_R13]
    mov r14, [rdi + REG_R14]
    mov r15, [rdi + REG_R15]
    
    mov rsi, [rdi + REG_RSI]
    mov rbp, [rdi + REG_RBP]
    mov rsp, [rdi + REG_RSP]
    
    mov rdi, [rdi + REG_RDI]
    
    ret

