; bare-metal/boot/boot.asm
; Multiboot2 header + x86 protected-mode startup stub.
; GRUB leaves us in 32-bit protected mode with A20 enabled, flat 32-bit segments,
; EBX = physical address of multiboot2 info structure.

bits 32

; ---- Multiboot2 constants ---------------------------------------------------
MB2_MAGIC    equ 0xE85250D6
MB2_ARCH     equ 0              ; i386 protected mode
MB2_HDRLEN   equ (header_end - header_start)
MB2_CHECKSUM equ (-(MB2_MAGIC + MB2_ARCH + MB2_HDRLEN) & 0xFFFFFFFF)

; ---- Multiboot2 header (must appear in first 32KB of the image) -------------
section .multiboot2
align 8
header_start:
    dd MB2_MAGIC
    dd MB2_ARCH
    dd MB2_HDRLEN
    dd MB2_CHECKSUM

    ; Framebuffer request tag (type=5, flags=0 = optional, size=20)
    ; Asks GRUB to set a linear pixel framebuffer.
    align 8
    dw 5            ; type: framebuffer
    dw 0            ; flags: 0 = optional (don't abort if unavailable)
    dd 20           ; size of this tag
    dd 1280         ; preferred width  (GRUB picks closest available)
    dd 720          ; preferred height
    dd 32           ; preferred bpp (32-bit ARGB)

    ; Terminating tag (type=0, flags=0, size=8)
    align 8
    dw 0
    dw 0
    dd 8
header_end:

; ---- Entry point ------------------------------------------------------------
section .text
global _start
_start:
    ; Save multiboot2 info pointer passed in EBX by GRUB.
    ; We push it as the argument to kmain(uint32_t mb_info_phys).
    mov  esp, stack_top
    push 0            ; clear EFLAGS
    popf

    push ebx          ; multiboot2 info pointer → first argument of kmain
    extern kmain
    call kmain

    ; kmain returned (shouldn't happen) — halt forever
.halt:
    cli
    hlt
    jmp .halt

; ---- Stack ------------------------------------------------------------------
section .bss
align 16
stack_bottom:
    resb 16384        ; 16 KB
stack_top:
