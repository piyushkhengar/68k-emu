; bare-metal/lib/setjmp_bm.asm
; Proper i386 setjmp / longjmp for bare-metal (no OS, no libc).
;
; jmp_buf layout (6 × 4 bytes = 24 bytes):
;   [+0 ] EBX
;   [+4 ] ESI
;   [+8 ] EDI
;   [+12] EBP
;   [+16] ESP  (value inside setjmp: ESP as seen by its caller)
;   [+20] EIP  (return address = instruction after the CALL setjmp)

bits 32
section .text

; int setjmp(jmp_buf env)
; Returns 0; after longjmp returns the val passed to longjmp.
global setjmp
setjmp:
    mov  ecx, [esp+4]       ; ecx = env pointer (first argument)
    mov  eax, [esp]         ; eax = return address
    mov  [ecx+0],  ebx
    mov  [ecx+4],  esi
    mov  [ecx+8],  edi
    mov  [ecx+12], ebp
    mov  [ecx+16], esp      ; save caller's ESP (frame before CALL setjmp)
    mov  [ecx+20], eax      ; save return address
    xor  eax, eax           ; return 0
    ret

; void longjmp(jmp_buf env, int val)  — does not return
global longjmp
longjmp:
    mov  ecx, [esp+4]       ; ecx = env pointer
    mov  eax, [esp+8]       ; eax = val
    test eax, eax
    jnz  .nonzero
    inc  eax                ; longjmp(env,0) → behaves as longjmp(env,1)
.nonzero:
    mov  ebx, [ecx+0]
    mov  esi, [ecx+4]
    mov  edi, [ecx+8]
    mov  ebp, [ecx+12]
    mov  esp, [ecx+16]      ; restore ESP (points to saved return address)
    jmp  dword [ecx+20]     ; jump back into setjmp's caller; eax = val
