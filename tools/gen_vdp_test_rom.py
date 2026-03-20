#!/usr/bin/env python3
"""
Generate a minimal Genesis test ROM that exercises the VDP:
  1. Read VDP status register
  2. Store it to Work RAM
  3. Configure VDP registers (mode, display, auto-increment)
  4. Write test pattern to VRAM via two-word control port protocol
  5. Write a backdrop color to CRAM
  6. Halt

Run:  python3 tools/gen_vdp_test_rom.py
Test: ./68k-emu --genesis test_genesis_vdp.bin --max-steps 1000
"""

import struct

rom = bytearray(2048)

# Genesis vector table
struct.pack_into('>I', rom, 0x00, 0x00FF0000)  # Initial SSP
struct.pack_into('>I', rom, 0x04, 0x00000200)  # Initial PC

pc = 0x200

# 1. Read VDP status register into D0  (MOVE.W (0xC00004).L, D0)
struct.pack_into('>H', rom, pc, 0x3039)
struct.pack_into('>I', rom, pc + 2, 0x00C00004)
pc += 6

# 2. Store status to Work RAM  (MOVE.W D0, (0xE00000).L)
struct.pack_into('>H', rom, pc, 0x33C0)
struct.pack_into('>I', rom, pc + 2, 0x00E00000)
pc += 6

# 3. VDP register 0 = 0x04  (MOVE.W #0x8004, (0xC00004).L)
struct.pack_into('>H', rom, pc, 0x33FC)
struct.pack_into('>H', rom, pc + 2, 0x8004)
struct.pack_into('>I', rom, pc + 4, 0x00C00004)
pc += 8

# 4. VDP register 1 = 0x74 (display on, VInt on, DMA on, Mode 5)
struct.pack_into('>H', rom, pc, 0x33FC)
struct.pack_into('>H', rom, pc + 2, 0x8174)
struct.pack_into('>I', rom, pc + 4, 0x00C00004)
pc += 8

# 5. VDP register 15 = 0x02 (auto-increment by 2)
struct.pack_into('>H', rom, pc, 0x33FC)
struct.pack_into('>H', rom, pc + 2, 0x8F02)
struct.pack_into('>I', rom, pc + 4, 0x00C00004)
pc += 8

# 6. Set up VRAM write at address 0x0000 -- first control word
#    Bits 15-14 = 01 (code 1-0), bits 13-0 = address
struct.pack_into('>H', rom, pc, 0x33FC)
struct.pack_into('>H', rom, pc + 2, 0x4000)
struct.pack_into('>I', rom, pc + 4, 0x00C00004)
pc += 8

# 7. Second control word (completes VRAM write setup)
struct.pack_into('>H', rom, pc, 0x33FC)
struct.pack_into('>H', rom, pc + 2, 0x0000)
struct.pack_into('>I', rom, pc + 4, 0x00C00004)
pc += 8

# 8. Write 0xDEAD to VRAM via data port
struct.pack_into('>H', rom, pc, 0x33FC)
struct.pack_into('>H', rom, pc + 2, 0xDEAD)
struct.pack_into('>I', rom, pc + 4, 0x00C00000)
pc += 8

# 9. Write 0xBEEF to VRAM (auto-incremented to address 2)
struct.pack_into('>H', rom, pc, 0x33FC)
struct.pack_into('>H', rom, pc + 2, 0xBEEF)
struct.pack_into('>I', rom, pc + 4, 0x00C00000)
pc += 8

# 10. Set up CRAM write at address 0x0000 -- first control word
#     Bits 15-14 = 11 (code 1-0 = 0x03 for CRAM write)
struct.pack_into('>H', rom, pc, 0x33FC)
struct.pack_into('>H', rom, pc + 2, 0xC000)
struct.pack_into('>I', rom, pc + 4, 0x00C00004)
pc += 8

# 11. Second control word
struct.pack_into('>H', rom, pc, 0x33FC)
struct.pack_into('>H', rom, pc + 2, 0x0000)
struct.pack_into('>I', rom, pc + 4, 0x00C00004)
pc += 8

# 12. Write green backdrop color (0x0E00) to CRAM entry 0
struct.pack_into('>H', rom, pc, 0x33FC)
struct.pack_into('>H', rom, pc + 2, 0x0E00)
struct.pack_into('>I', rom, pc + 4, 0x00C00000)
pc += 8

# 13. Copy status to D1  (MOVE.W D0, D1)
struct.pack_into('>H', rom, pc, 0x3200)
pc += 2

# 14. STOP #0x2700
struct.pack_into('>H', rom, pc, 0x4E72)
struct.pack_into('>H', rom, pc + 2, 0x2700)
pc += 4

with open('test_genesis_vdp.bin', 'wb') as f:
    f.write(rom)

print(f'Created test_genesis_vdp.bin ({len(rom)} bytes, {pc - 0x200} bytes of code at 0x200)')
