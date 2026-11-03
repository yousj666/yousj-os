/* Yousj OS - link layout for the 32-bit Multiboot stub (ELF32).
 * Linked at 12 MiB: above the kernel's 1 MiB load address with
 * room for the kernel image to grow into (its segments are
 * copied to 1 MiB before the stub's tables exist; at 4 MiB the
 * copy overran the stub itself once the image span passed
 * ~4.3 MiB, M59), and inside the first-16-MiB range that
 * mb1_build_boot_info() reserves, so the kernel PMM never
 * reallocates the stub's pages while the boot_info buffer
 * in its BSS is still being consumed at kmain entry.
 */
ENTRY(_stub_start)

SECTIONS
{
    . = 0xC00000;

    .multiboot ALIGN(4) :
    {
        *(.multiboot)
    }

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
