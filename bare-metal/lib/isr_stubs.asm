; bare-metal/lib/isr_stubs.asm
; Minimal ISR stubs for hardware IRQs and CPU exceptions.

bits 32
section .text

%define COM1 0x3F8

; ---- Helper: write character in AL to COM1 ----------------------------------
serial_char:
    mov dx, COM1
    out dx, al
    ret

; ---- Generic handler for primary PIC IRQs (vectors 32-39) -------------------
global isr_stub_primary
isr_stub_primary:
    pusha
    extern irq_handler_primary
    call irq_handler_primary
    popa
    iret

; ---- Generic handler for secondary PIC IRQs (vectors 40-47) -----------------
global isr_stub_secondary
isr_stub_secondary:
    pusha
    extern irq_handler_secondary
    call irq_handler_secondary
    popa
    iret

; ---- SB16 IRQ handler (IRQ5, vector 37) -------------------------------------
global isr_stub_sb16
isr_stub_sb16:
    pusha
    extern irq_handler_sb16
    call irq_handler_sb16
    popa
    iret

; ---- CPU exception handler (no error code) ----------------------------------
; Write 'E' to serial and halt.
global isr_stub_exception
isr_stub_exception:
    mov al, 'E'
    call serial_char
    mov al, '!'
    call serial_char
    mov al, 10
    call serial_char
    cli
    hlt
    jmp isr_stub_exception

; ---- CPU exception handler (with error code) --------------------------------
global isr_stub_exception_err
isr_stub_exception_err:
    add esp, 4
    mov al, 'E'
    call serial_char
    mov al, '#'
    call serial_char
    mov al, 10
    call serial_char
    cli
    hlt
    jmp isr_stub_exception_err
