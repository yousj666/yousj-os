/* Yousj OS - kernel link layout for the QEMU Multiboot smoke boot.
 * Segments are loaded at physical 1 MiB by the 32-bit stub's ELF
 * loader (stub.c) and identity-mapped by the stub's early tables.
 * The product layout (higher-half, -2 GiB kernel base per
 * docs/VMM_DESIGN.md) arrives with the UEFI bootloader chain;
 * this script is bring-up only.
 */
ENTRY(_kentry64)

SECTIONS
{
    . = 0x100000;

    .text ALIGN(4096) :
    {
        *(.text)
    }

    .rodata ALIGN(4096) :
    {
        *(.rodata)
    }

    .data ALIGN(4096) :
    {
        *(.data)
    }

    .bss ALIGN(4096) :
    {
        *(.bss)
        *(COMMON)
    }
}
