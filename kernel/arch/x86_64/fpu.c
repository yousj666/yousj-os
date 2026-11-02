/* Yousj OS - x87/SSE state management (M8). See <yousj/fpu.h>
 * for the model. Written by Muse (coordinator). */
#include <yousj/types.h>
#include <yousj/fpu.h>

#define CR0_MP (1ULL << 1)
#define CR0_EM (1ULL << 2)
#define CR0_TS (1ULL << 3)
#define CR0_NE (1ULL << 5)
#define CR4_OSFXSR     (1ULL << 9)
#define CR4_OSXMMEXCPT (1ULL << 10)

#define MXCSR_DEFAULT 0x1F80u /* exceptions masked, round-nearest */
#define FX_OFF_MXCSR  24      /* offset of MXCSR in the FX area */

static uint8_t g_pristine[YOUSJ_FX_SIZE] __attribute__((aligned(16)));
static int g_fpu_ready;

static uint64_t read_cr0(void)
{
    uint64_t v;
    __asm__ volatile("mov %%cr0, %0" : "=r"(v));
    return v;
}

static void write_cr0(uint64_t v)
{
    __asm__ volatile("mov %0, %%cr0" : : "r"(v) : "memory");
}

static uint64_t read_cr4(void)
{
    uint64_t v;
    __asm__ volatile("mov %%cr4, %0" : "=r"(v));
    return v;
}

static void write_cr4(uint64_t v)
{
    __asm__ volatile("mov %0, %%cr4" : : "r"(v) : "memory");
}

static void mem_copy(uint8_t *dst, const uint8_t *src, uint64_t n)
{
    uint64_t i;
    for (i = 0; i < n; i++)
        dst[i] = src[i];
}

void fpu_save(uint8_t *area)
{
    __asm__ volatile("fxsave64 (%0)" : : "r"(area) : "memory");
}

void fpu_restore(const uint8_t *area)
{
    __asm__ volatile("fxrstor64 (%0)" : : "r"(area) : "memory");
}

void fpu_init_cpu(void)
{
    uint64_t v;
    uint8_t seed[YOUSJ_FX_SIZE] __attribute__((aligned(16)));
    uint64_t i;

    v = read_cr0();
    v &= ~(CR0_EM | CR0_TS); /* no emulation, no lazy switching */
    v |= CR0_MP | CR0_NE;
    write_cr0(v);

    v = read_cr4();
    v |= CR4_OSFXSR | CR4_OSXMMEXCPT;
    write_cr4(v);

    /* Build the pristine image deterministically: start from a
     * zeroed area carrying only the MXCSR default, load it
     * (which also resets the x87 side to post-fninit state via
     * the zero FCW/FTW fields -- then run fninit to be exact),
     * and capture the result as the canonical image. */
    for (i = 0; i < YOUSJ_FX_SIZE; i++)
        seed[i] = 0;
    *(uint32_t *)(seed + FX_OFF_MXCSR) = MXCSR_DEFAULT;
    fpu_restore(seed);
    __asm__ volatile("fninit");
    /* fninit does not touch MXCSR; the fxrstor above set it.
     * Capture the combined state. */
    fpu_save(g_pristine);
    g_fpu_ready = 1;
}

void fpu_init_area(uint8_t *area)
{
    if (!g_fpu_ready) {
        /* fpu_init_cpu() has not run (host unit tests link this
         * weakly and skip it): leave the zeroed area alone. */
        uint64_t i;
        for (i = 0; i < YOUSJ_FX_SIZE; i++)
            area[i] = 0;
        return;
    }
    mem_copy(area, g_pristine, YOUSJ_FX_SIZE);
}
