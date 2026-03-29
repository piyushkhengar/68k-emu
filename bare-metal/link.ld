/* bare-metal/link.ld
 * Linker script for the bare-metal 68k-emu kernel.
 * Loaded by GRUB at physical address 0x100000 (1 MB mark).
 */

ENTRY(_start)

SECTIONS
{
    . = 0x100000;

    /* Multiboot2 header must be first */
    .multiboot2 : {
        *(.multiboot2)
    }

    .text ALIGN(4096) : {
        *(.text*)
    }

    .rodata ALIGN(4096) : {
        *(.rodata*)
    }

    /* Embedded ROM bytes — placed in read-only data, own section for clarity */
    .rom_data ALIGN(4096) : {
        *(.rom_data)
    }

    .data ALIGN(4096) : {
        *(.data*)
    }

    .bss ALIGN(4096) : {
        *(COMMON)
        *(.bss*)

        /* 20 MB bump-allocator heap.
         * Covers the 16 MB emulator RAM (from mem_init calloc) plus overhead.
         * BSS is zero-initialised by GRUB at load time — no binary bloat. */
        . = ALIGN(16);
        heap_start = .;
        . += 20 * 1024 * 1024;
        heap_end = .;
    }

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
        *(.gcc_except_table*)
    }
}
