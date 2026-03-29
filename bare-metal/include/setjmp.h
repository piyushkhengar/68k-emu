/* bare-metal/include/setjmp.h — shadow header for freestanding build.
 * Provides a proper jmp_buf (6 × uintptr_t) and declares setjmp/longjmp.
 * The implementations live in bare-metal/lib/setjmp_bm.asm, which saves
 * EBX, ESI, EDI, EBP, ESP, EIP — all callee-saved registers needed for
 * correct unwinding in the 68k emulator's exception-handling path. */

#ifndef _BM_SETJMP_H
#define _BM_SETJMP_H

#include <stdint.h>

/* jmp_buf layout (matches setjmp_bm.asm):
 *   [0] EBX  [1] ESI  [2] EDI  [3] EBP  [4] ESP  [5] EIP  */
typedef uintptr_t jmp_buf[6];

int  setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val);

#endif /* _BM_SETJMP_H */
