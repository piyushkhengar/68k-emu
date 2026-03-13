#!/usr/bin/env python3
"""
Patch MCL68 test ROM for assembler encoding bugs.
Apply to mcl68_test.bin (backup as mcl68_test.bin.bak first).

Fixes:
1. 0x307C 0x0100 -> 0x323C 0x0100: MOVEA.W #0x100,A0 (was MOVE.W #imm,D7)
2. 0x10BC 0x0081 -> 0x143C 0x0081: MOVE.B #0x81,(A0) (was MOVE.B #imm,D2)
3. 0x1210 -> 0x1050: MOVE.B (A0),D1 (was storing to A0 instead of D1)
4. 0x10FC -> 0x163C: MOVE.B #imm,(A0)+ (was MOVE.B #imm,D3)
"""
import sys

def main():
    try:
        with open('mcl68_test.bin', 'rb') as f:
            data = bytearray(f.read())
    except FileNotFoundError:
        print("mcl68_test.bin not found")
        sys.exit(1)

    patches = [
        (bytes([0x30, 0x7C, 0x01, 0x00]), bytes([0x32, 0x3C, 0x01, 0x00]), "MOVEA.W #0x100,A0"),
        (bytes([0x10, 0xBC, 0x00, 0x81]), bytes([0x14, 0x3C, 0x00, 0x81]), "MOVE.B #0x81,(A0)"),
        (bytes([0x12, 0x10]), bytes([0x10, 0x50]), "MOVE.B (A0),D1"),
        (bytes([0x10, 0xFC]), bytes([0x16, 0x3C]), "MOVE.B #imm,(A0)+"),
    ]
    total = 0
    for old, new, desc in patches:
        count = 0
        i = 0
        while i <= len(data) - len(old):
            if data[i:i+len(old)] == old:
                data[i:i+len(old)] = new
                count += 1
            i += 1
        if count:
            print(f"  {desc}: {count} patches")
            total += count
    with open('mcl68_test.bin', 'wb') as f:
        f.write(data)
    print(f"Total: {total} patches applied")

if __name__ == '__main__':
    main()
