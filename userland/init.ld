/* Yousj OS - linker script for the init ELF (and later freestanding
 * user programs). Places the image at YOUSJ_USER_BASE so it lives in
 * the user window slot the kernel reserves for it (PML4 index 1,
 * far away from the kernel's identity subtree at index 0). Explicit
 * PHDRS keep text/rodata/data in separate PT_LOAD segments so the
 * kernel loader can map them with distinct permissions (W^X). */
ENTRY(_start)
PHDRS
{
    text   PT_LOAD FLAGS(5); /* R+X */
    rodata PT_LOAD FLAGS(4); /* R */
    data   PT_LOAD FLAGS(6); /* R+W */
}
SECTIONS
{
    . = 0x8000000000;
    .text : { *(.text) *(.text.*) } :text
    . = ALIGN(0x1000);
    .rodata : { *(.rodata) *(.rodata.*) *(.eh_frame) *(.note*) } :rodata
    . = ALIGN(0x1000);
    .data : { *(.data) *(.data.*) } :data
    .bss : { *(.bss) *(.bss.*) *(COMMON) } :data
    /DISCARD/ : { *(.comment) }
}
