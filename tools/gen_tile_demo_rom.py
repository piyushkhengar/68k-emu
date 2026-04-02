#!/usr/bin/env python3
"""
Generate a Genesis ROM that demonstrates tile and plane rendering.

Creates 6 tile patterns, loads a colorful 16-color palette, fills Plane A
with a cycling tile pattern and Plane B with a repeating frame tile.
The backdrop color shows through transparent regions.

Run:  python3 tools/gen_tile_demo_rom.py
Play: make genesis && ./68k-emu --genesis test_tile_demo.bin
"""

import struct

rom = bytearray(8192)

struct.pack_into('>I', rom, 0x00, 0x00FF0000)   # Initial SSP
struct.pack_into('>I', rom, 0x04, 0x00000200)   # Initial PC

pc = 0x200

# ---- Helpers for emitting 68K machine code ----

def emit(*words):
    global pc
    for w in words:
        struct.pack_into('>H', rom, pc, w & 0xFFFF)
        pc += 2

def vdp_reg(reg, val):
    emit(0x33FC, 0x8000 | (reg << 8) | val, 0x00C0, 0x0004)

def vdp_ctrl(word):
    emit(0x33FC, word, 0x00C0, 0x0004)

def vdp_data(word):
    emit(0x33FC, word, 0x00C0, 0x0000)

def setup_vram_write(addr):
    vdp_ctrl((addr & 0x3FFF) | 0x4000)
    vdp_ctrl((addr >> 14) & 0x03)

def setup_cram_write(addr):
    vdp_ctrl((addr & 0x3FFF) | 0xC000)
    vdp_ctrl((addr >> 14) & 0x03)

def gen_color(r, g, b):
    return ((b & 7) << 9) | ((g & 7) << 5) | ((r & 7) << 1)

# ---------------------------------------------------------------
#  1. Configure VDP registers
# ---------------------------------------------------------------
vdp_reg(0,  0x04)    # Normal mode, HInt disabled
vdp_reg(1,  0x64)    # Display ON, VInt ON, DMA ON, mode 5
vdp_reg(2,  0x30)    # Plane A nametable at VRAM 0xC000
vdp_reg(4,  0x07)    # Plane B nametable at VRAM 0xE000
vdp_reg(7,  0x09)    # Backdrop = CRAM index 9 (light blue)
vdp_reg(10, 0xFF)    # HInt counter max (HInt effectively disabled)
vdp_reg(11, 0x00)    # Full-screen scroll mode
vdp_reg(12, 0x81)    # H40 mode (320 pixels wide)
vdp_reg(13, 0x00)    # H-scroll table at VRAM 0x0000 (all zero = no scroll)
vdp_reg(15, 0x02)    # Auto-increment by 2 bytes
vdp_reg(16, 0x01)    # Plane size: 64 wide x 32 tall

# ---------------------------------------------------------------
#  2. Write 6 tile patterns to VRAM
#     Each pattern = 32 bytes (8 rows x 4 bytes/row, 4bpp packed)
#     Pattern index 0 is always "empty" (VRAM zeroed at init)
# ---------------------------------------------------------------

# Pattern 1 (VRAM 0x0020): Solid block, all pixels = color index 1
setup_vram_write(0x0020)
for _ in range(16):
    vdp_data(0x1111)

# Pattern 2 (VRAM 0x0040): Checkerboard, alternating indices 2 and 3
setup_vram_write(0x0040)
for row in range(8):
    if row % 2 == 0:
        vdp_data(0x2323); vdp_data(0x2323)
    else:
        vdp_data(0x3232); vdp_data(0x3232)

# Pattern 3 (VRAM 0x0060): Horizontal stripes, indices 4 (top) / 5 (bottom)
setup_vram_write(0x0060)
for row in range(8):
    if row < 4:
        vdp_data(0x4444); vdp_data(0x4444)
    else:
        vdp_data(0x5555); vdp_data(0x5555)

# Pattern 4 (VRAM 0x0080): Vertical stripes, indices 6 and 1
setup_vram_write(0x0080)
for _ in range(8):
    vdp_data(0x6161); vdp_data(0x6161)

# Pattern 5 (VRAM 0x00A0): Diamond, index 7 on transparent
setup_vram_write(0x00A0)
for w in [0x0007, 0x7000,
          0x0077, 0x7700,
          0x0777, 0x7770,
          0x7777, 0x7777,
          0x7777, 0x7777,
          0x0777, 0x7770,
          0x0077, 0x7700,
          0x0007, 0x7000]:
    vdp_data(w)

# Pattern 6 (VRAM 0x00C0): Border frame, index 8 on transparent
setup_vram_write(0x00C0)
for w in [0x8888, 0x8888,
          0x8000, 0x0008,
          0x8000, 0x0008,
          0x8000, 0x0008,
          0x8000, 0x0008,
          0x8000, 0x0008,
          0x8000, 0x0008,
          0x8888, 0x8888]:
    vdp_data(w)

# ---------------------------------------------------------------
#  3. Write palette (16 colors) to CRAM
# ---------------------------------------------------------------
setup_cram_write(0x0000)
for c in [
    gen_color(1, 1, 1),    #  0: very dark gray (transparent index)
    gen_color(0, 2, 7),    #  1: deep blue
    gen_color(0, 7, 2),    #  2: green
    gen_color(7, 2, 0),    #  3: red
    gen_color(7, 7, 0),    #  4: yellow
    gen_color(7, 0, 7),    #  5: magenta
    gen_color(0, 7, 7),    #  6: cyan
    gen_color(7, 7, 7),    #  7: white
    gen_color(4, 4, 4),    #  8: mid gray
    gen_color(2, 3, 6),    #  9: light blue (backdrop)
    gen_color(5, 5, 5),    # 10: light gray
    gen_color(7, 5, 0),    # 11: orange
    gen_color(3, 0, 5),    # 12: purple
    gen_color(0, 5, 3),    # 13: teal
    gen_color(7, 7, 5),    # 14: cream
    gen_color(6, 6, 6),    # 15: near-white
]:
    vdp_data(c)

# ---------------------------------------------------------------
#  4. Fill Plane A nametable at VRAM 0xC000 using a 68K loop
#     Cycles tiles 1-6 across all 64x32 = 2048 entries
# ---------------------------------------------------------------
setup_vram_write(0xC000)

#   MOVEQ  #1, D1          ; starting tile index
emit(0x7201)
#   MOVE.W #2047, D0       ; DBRA counter (64*32 - 1)
emit(0x303C, 0x07FF)

# .loop:
loop_a = pc
#   MOVE.W D1, (0xC00000).L   ; write tile entry to VDP data port
emit(0x33C1, 0x00C0, 0x0000)
#   ADDQ.W #1, D1
emit(0x5241)
#   CMPI.W #7, D1             ; past tile 6?
emit(0x0C41, 0x0007)
#   BNE.S  +2                 ; skip reset if not
emit(0x6602)
#   MOVEQ  #1, D1             ; wrap back to tile 1
emit(0x7201)
#   DBRA   D0, .loop
emit(0x51C8, (loop_a - (pc + 2)) & 0xFFFF)

# ---------------------------------------------------------------
#  5. Fill Plane B nametable at VRAM 0xE000 with tile 6 (frame)
#     Frame has transparent interior → backdrop shows through
# ---------------------------------------------------------------
setup_vram_write(0xE000)

#   MOVE.W #2047, D0
emit(0x303C, 0x07FF)

# .loop_b:
loop_b = pc
#   MOVE.W #0x0006, (0xC00000).L   ; tile 6 everywhere
emit(0x33FC, 0x0006, 0x00C0, 0x0000)
#   DBRA   D0, .loop_b
emit(0x51C8, (loop_b - (pc + 2)) & 0xFFFF)

# ---------------------------------------------------------------
#  6. Lower interrupt mask and spin forever
# ---------------------------------------------------------------
#   MOVE.W #0x2000, SR
emit(0x46FC, 0x2000)
#   BRA.S  *                  ; infinite loop
emit(0x60FE)

# ---------------------------------------------------------------
with open('test_tile_demo.bin', 'wb') as f:
    f.write(rom)

print(f'Created test_tile_demo.bin ({len(rom)} bytes)')
print(f'  Code: {pc - 0x200} bytes at 0x200 (ends at 0x{pc:04X})')
print(f'  6 tile patterns, 16-color palette')
print(f'  Plane A: tiles 1-6 cycling diagonal')
print(f'  Plane B: tile 6 (frame) everywhere')
print(f'  Backdrop: light blue (CRAM[9])')
