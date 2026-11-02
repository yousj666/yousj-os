/* Yousj OS - kernel link layout (M6: higher-half kernel).
 *
 * Virtual layout: the kernel image lives at YOUSJ_KERNEL_BASE
 * (0xFFFFFFFF80000000, the -2 GiB slot) per docs/VMM_DESIGN.md
 * sec. 3. Physical layout: loaded at 1 MiB, exactly as before --
 * both boot chains load PT_LOAD segments by p_paddr, build early
 * page tables that map the image at its virtual base, and only
 * then jump to the (virtual) entry point. __kernel_start/end and
 * the per-section bounds are consumed by vmm_hw_init(), which
 * rebuilds these mappings with real permissions in the kernel
 * master space. The V2P delta is 1 MiB-aligned, so p_vaddr and
 * p_paddr stay congruent modulo the page size as ELF requires. */

ENTRY(_kentry64)

KERNEL_V2P_DELTA = 0xFFFFFFFF7FF00000; /* 0xFFFFFFFF80000000 - 0x100000 */

SECTIONS
{
    . = 0xFFFFFFFF80000000;

    __kernel_start = .;

    .text ALIGN(4096) : AT(ADDR(.text) - KERNEL_V2P_DELTA)
    {
        __text_start = .;
        *(.text)
        __text_end = .;
    }

    .rodata ALIGN(4096) : AT(ADDR(.rodata) - KERNEL_V2P_DELTA)
    {
        __rodata_start = .;
        *(.rodata)
        __rodata_end = .;
    }

    .data ALIGN(4096) : AT(ADDR(.data) - KERNEL_V2P_DELTA)
    {
        __data_start = .;
        *(.data)
        __data_end = .;
    }

    .bss ALIGN(4096) : AT(ADDR(.bss) - KERNEL_V2P_DELTA)
    {
        __bss_start = .;
        *(.bss)
        *(COMMON)
        __bss_end = .;
    }

    __kernel_end = .;
}
