/* Yousj OS - static ELF64 user-image loader (stage 1, M4).
 *
 * Written from scratch by Muse (coordinator), per
 * docs/SCHED_DESIGN.md sec. 7.3 step 2: the minimal honest subset --
 * ELF64, little-endian, x86-64, PT_LOAD segments only.
 * ET_EXEC loads at its linked addresses; ET_DYN (static
 * PIE, M56) loads at the user-window base and relocates
 * itself from AT_PHDR. PT_INTERP images are supported from
 * M58: elf_scan_interp reports the interpreter path and the
 * exec layer loads that image too, at a caller-chosen bias,
 * handing it control with AT_BASE set.
 * The image bytes come from
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
                            uint64_t phdr_va, uint64_t phnum,
                            uint64_t entry, uint64_t interp_base,
                            uint64_t *sp_out);

/* bias_req: an explicit load bias for ET_DYN images (the
 * dynamic loader's slot, M58), or ~0ULL for the default
 * placement (ET_DYN at the window base, ET_EXEC at its
 * linked addresses). */
int elf_load_user_image(const uint8_t *image, uint64_t size,
                        struct vmm_space *space, uint64_t bias_req,
                        uint64_t *entry_out, uint64_t *brk_out,
                        uint64_t *phdr_out, uint64_t *phnum_out);

/* M58: copy the PT_INTERP path (NUL-terminated) into buf.
 * Returns its length, or -1 when the image has none. */
long elf_scan_interp(const uint8_t *image, uint64_t size,
                     char *buf, uint64_t cap);

#endif /* YOUSJ_ELF_H */
