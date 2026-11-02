/* Yousj OS - static ELF64 user-image loader (stage 1, M4).
 *
 * Written from scratch by Muse (coordinator), per
 * docs/SCHED_DESIGN.md sec. 7.3 step 2: the minimal honest subset --
 * ELF64, little-endian, ET_EXEC, x86-64, PT_LOAD segments only.
 * No dynamic linking, no PIE relocation, no interpreter: anything
 * fancier is rejected, not half-loaded. The image bytes come from
 * memory (in stage 1, an init ELF embedded in the kernel image;
 * the same entry point serves a ramdisk image later).
 *
 * Loading rules:
 *   - every segment must land inside the user window
 *     [YOUSJ_USER_BASE, YOUSJ_USER_LIMIT);
 *   - pages are allocated per segment and zero-filled first, so
 *     the bss tail (memsz > filesz) is automatically clean;
 *   - permissions come from the segment (this is where W^X
 *     separation enters: text is RX, data is RW, never both);
 *   - a page shared by two segments keeps the UNION of their
 *     permissions and the later segment's bytes win overlaps.
 */
#ifndef YOUSJ_ELF_H
#define YOUSJ_ELF_H

#include <yousj/types.h>

struct vmm_space;

/* Load the ELF image at [image, image+size) into `space`.
 * On success returns 0 and stores the entry point plus the
 * initial program break (end of the highest segment, the heap
 * base a brk(0) should report). On any inconsistency returns -1
 * and the space may hold partial mappings (callers discard it). */
/* Build the Linux initial stack in a fresh space: 4 pages
 * below stack_top, argument/environment strings packed at the
 * top, then the [argc][argv...][NULL][envp...][NULL][auxv]
 * block (AT_PAGESZ + AT_NULL). argv/envp are NULL-terminated
 * arrays of kernel-side strings (either may be NULL). On
 * success *sp_out is the initial RSP. Shared by the boot init
 * wiring and execve (M5). */
int elf_build_initial_stack(struct vmm_space *space,
                            uint64_t stack_top,
                            const char *const *argv,
                            const char *const *envp,
                            uint64_t *sp_out);

int elf_load_user_image(const uint8_t *image, uint64_t size,
                        struct vmm_space *space,
                        uint64_t *entry_out, uint64_t *brk_out);

#endif /* YOUSJ_ELF_H */
