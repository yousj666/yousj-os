/* Yousj OS - M8 FPU/SSE acceptance suite (userland).
 *
 * Proves the M8 floating-point model end to end:
 *   basic        scalar SSE2 double math is bit-exact
 *   isolation    two processes hold DISTINCT values in xmm0-3
 *                across nanosleep (i.e. across context switches)
 *                and both get their own values back
 *   mxcsr        SSE divide-by-zero yields +inf (no #XM): the
 *                pristine per-task state masks exceptions
 *   sigfp        a signal frame carries a real fpstate pointer;
 *                the handler reads the delivered xmm0 from it,
 *                clobbers xmm0, and sigreturn restores the
 *                original value
 *   exec-reset   mode "fxchild" is exec'd by T5 with a dirty
 *                (unmasked) MXCSR; its divide-by-zero must
 *                still be quiet, proving exec resets FP state
 *
 * The values-in-registers checks are single inline-asm blocks:
 * nothing the compiler does can spill or reload the xmm
 * registers inside them.
 *
 * Prints "FXTEST <name> PASS"/"FXTEST <name> FAIL detail=" then
 * "FXTEST DONE"; exit status 0 iff all checks passed.
 */
#include <stdint.h>
#include <stddef.h>

#define SYS_read         0
#define SYS_write        1
#define SYS_clone        56
#define SYS_execve       59
#define SYS_wait4        61
#define SYS_kill         62
#define SYS_getpid       39
#define SYS_exit_group   231
#define SYS_nanosleep    35
#define SYS_rt_sigaction 13
#define SYS_rt_sigreturn 15

#define SIGUSR1 10
#define SIGCHLD 17

#define FX_PAT_A0 0x3FF8000000000000ULL /* 1.5   */
#define FX_PAT_A1 0x4002000000000000ULL /* 2.25  */
#define FX_PAT_A2 0x400A000000000000ULL /* 3.25  */
#define FX_PAT_A3 0x4012000000000000ULL /* 4.5   */
#define FX_PAT_B0 0x4020000000000000ULL /* 8.0   */
#define FX_PAT_B1 0x4026000000000000ULL /* 11.0  */
#define FX_PAT_B2 0x402A000000000000ULL /* 13.0  */
#define FX_PAT_B3 0x4031000000000000ULL /* 17.0  */
#define FX_CLOBBER 0xDEADBEEF12345678ULL

struct k_timespec { long sec; long nsec; };
struct k_sigaction {
    uint64_t handler;
    uint32_t flags;
    uint32_t reserved;
    uint64_t restorer;
    uint64_t mask;
};

static long sys_call3(long nr, long a1, long a2, long a3)
{
    long ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return ret;
}

static long sys_call4(long nr, long a1, long a2, long a3, long a4)
{
    long ret;
    register long r10 __asm__("r10") = a4;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr), "D"(a1), "S"(a2), "d"(a3), "r"(r10)
                     : "rcx", "r11", "memory");
    return ret;
}

static void out(const char *s)
{
    long n = 0;
    while (s[n])
        n++;
    if (n > 0)
        sys_call3(SYS_write, 1, (long)(uintptr_t)s, n);
}

static void out_long(long v)
{
    char buf[24];
    int i = 0;
    unsigned long u;
    if (v < 0) {
        out("-");
        u = (unsigned long)(-(v + 1)) + 1;
    } else {
        u = (unsigned long)v;
    }
    if (u == 0) {
        out("0");
        return;
    }
    while (u > 0) {
        buf[i++] = (char)('0' + (int)(u % 10));
        u /= 10;
    }
    while (i > 0) {
        char c[2];
        c[0] = buf[--i];
        c[1] = '\0';
        out(c);
    }
}

static int g_fails;
static void report(const char *name, int ok)
{
    out("FXTEST ");
    out(name);
    out(ok ? " PASS\n" : " FAIL\n");
    if (!ok)
        g_fails++;
}

static void report2(const char *name, int ok, long detail)
{
    out("FXTEST ");
    out(name);
    out(ok ? " PASS" : " FAIL detail=");
    if (!ok)
        out_long(detail);
    out("\n");
    if (!ok)
        g_fails++;
}

static void exit_now(long code)
{
    sys_call3(SYS_exit_group, code, 0, 0);
    for (;;) {
    }
}

static long fork_(void)
{
    return sys_call3(SYS_clone, 17 /* SIGCHLD */, 0, 0);
}

static long wait4_(long pid, long *status)
{
    return sys_call4(SYS_wait4, pid, (long)(uintptr_t)status, 0, 0);
}

/* FXSAVE area XMM offsets: xmm0 at 160, 16 bytes each;
 * MXCSR (u32) at 24. */
#define FXO_XMM0 160
#define FXO_MXCSR 24

/* Load the four pattern constants into xmm0-3, nanosleep (a
 * full context-switch round trip), then store xmm0-3 back out.
 * Runs entirely inside one asm block. */
static void patterns_sleep_extract(const uint64_t pats[4], long ms,
                                   uint64_t outv[4])
{
    struct k_timespec ts;
    ts.sec = ms / 1000;
    ts.nsec = (ms % 1000) * 1000000L;
    __asm__ volatile(
        "movq (%0), %%xmm0\n\t"
        "movq 8(%0), %%xmm1\n\t"
        "movq 16(%0), %%xmm2\n\t"
        "movq 24(%0), %%xmm3\n\t"
        "mov %2, %%rdi\n\t"
        "xor %%esi, %%esi\n\t"
        "mov $35, %%rax\n\t"
        "syscall\n\t"
        "movq %%xmm0, (%1)\n\t"
        "movq %%xmm1, 8(%1)\n\t"
        "movq %%xmm2, 16(%1)\n\t"
        "movq %%xmm3, 24(%1)\n\t"
        :
        : "r"(pats), "r"(outv), "r"((uintptr_t)&ts)
        : "rax", "rdi", "rsi", "rcx", "r11", "xmm0", "xmm1",
          "xmm2", "xmm3", "memory");
}

/* ---- T1: scalar SSE2 arithmetic, bit-exact ---- */
static void t_basic(void)
{
    volatile double a = 1.5;
    volatile double b = 2.25;
    volatile double r;
    union { double d; uint64_t u; } cvt;
    uint64_t bits;
    r = a * b + a; /* 3.375 + 1.5 = 4.875 */
    cvt.d = r;
    bits = cvt.u;
    report2("basic", bits == 0x4013800000000000ULL, (long)bits);
}

/* ---- T2: per-task isolation across context switches ---- */
static void t_isolation(void)
{
    static const uint64_t pa[4] = { FX_PAT_A0, FX_PAT_A1, FX_PAT_A2, FX_PAT_A3 };
    static const uint64_t pb[4] = { FX_PAT_B0, FX_PAT_B1, FX_PAT_B2, FX_PAT_B3 };
    uint64_t got[4] = { 0, 0, 0, 0 };
    long pid, st = -1, r;
    int ok, i;

    pid = fork_();
    if (pid == 0) {
        uint64_t cgot[4] = { 0, 0, 0, 0 };
        patterns_sleep_extract(pb, 400, cgot);
        for (i = 0; i < 4; i++)
            if (cgot[i] != pb[i])
                exit_now(10 + i);
        exit_now(0);
    }
    if (pid < 0) {
        report("isolation", 0);
        return;
    }
    patterns_sleep_extract(pa, 400, got);
    r = wait4_(pid, &st);
    ok = (r == pid) && ((st & 0xFFFFFFFFL) == 0);
    for (i = 0; i < 4; i++)
        if (got[i] != pa[i])
            ok = 0;
    report2("isolation", ok, st & 0xFFFFFFFFL);
}

/* ---- T3: pristine MXCSR masks SSE exceptions ---- */
static void t_mxcsr(void)
{
    uint64_t res = 0;
    __asm__ volatile(
        "movq %1, %%xmm0\n\t"   /* 1.0 */
        "movq %2, %%xmm1\n\t"   /* 0.0 */
        "divsd %%xmm1, %%xmm0\n\t" /* +inf, or #XM if unmasked */
        "movq %%xmm0, %0\n\t"
        : "=m"(res)
        : "m"(*(const uint64_t[]){ 0x3FF0000000000000ULL }),
          "m"(*(const uint64_t[]){ 0ULL })
        : "xmm0", "xmm1");
    report2("mxcsr", res == 0x7FF0000000000000ULL, (long)res);
}

/* ---- T4: signal frame fpstate is real; sigreturn restores -- */
static volatile uint64_t g_sig_fpstate;
static volatile uint64_t g_sig_xmm0;
static volatile uint32_t g_sig_mxcsr;

static void usr1_handler(long sig, void *info, void *uctx)
{
    const uint8_t *uc = (const uint8_t *)uctx;
    uint64_t fp;
    (void)sig;
    (void)info;
    /* ucontext.mcontext.fpstate: mcontext at uc+40, fpstate at
     * mcontext+192 (kernel offsetof-verified). */
    fp = *(const uint64_t *)(const void *)(uc + 40 + 192);
    g_sig_fpstate = fp;
    if (fp != 0) {
        const uint8_t *fx = (const uint8_t *)(uintptr_t)fp;
        g_sig_xmm0 = *(const uint64_t *)(const void *)(fx + FXO_XMM0);
        g_sig_mxcsr = *(const uint32_t *)(const void *)(fx + FXO_MXCSR);
    }
    /* Clobber xmm0: if sigreturn restores the frame state, the
     * main line must NOT see this value afterwards. */
    __asm__ volatile("movq %0, %%xmm0"
                     :
                     : "m"(*(const uint64_t[]){ FX_CLOBBER })
                     : "xmm0");
}

__asm__(
    ".globl fx_restorer\n"
    "fx_restorer:\n"
    "    mov $15, %rax\n" /* rt_sigreturn */
    "    syscall\n"
    "    ud2\n");

static void t_sigfp(void)
{
    extern char fx_restorer[];
    struct k_sigaction act;
    uint64_t got = 0;
    long mypid;
    int ok;

    act.handler = (uint64_t)(uintptr_t)usr1_handler;
    act.flags = 0x04000004u /* SA_RESTORER | SA_SIGINFO */;
    act.reserved = 0;
    act.restorer = (uint64_t)(uintptr_t)fx_restorer;
    act.mask = 0;
    if (sys_call4(SYS_rt_sigaction, SIGUSR1, (long)(uintptr_t)&act, 0, 8) != 0) {
        report("sigfp", 0);
        return;
    }
    mypid = sys_call3(SYS_getpid, 0, 0, 0);
    /* Put PAT_A0 in xmm0, raise the signal, and read xmm0 back
     * in one asm block so the compiler cannot disturb it. */
    __asm__ volatile(
        "movq %2, %%xmm0\n\t"
        "mov %3, %%rdi\n\t"
        "mov $10, %%rsi\n\t" /* SIGUSR1 */
        "mov $62, %%rax\n\t" /* kill */
        "syscall\n\t"
        "movq %%xmm0, %0\n\t"
        : "=m"(got)
        : "m"(*(const uint64_t[]){ 0ULL }),
          "m"(*(const uint64_t[]){ FX_PAT_A0 }),
          "r"(mypid)
        : "rax", "rdi", "rsi", "rcx", "r11", "xmm0", "memory");
    ok = (got == FX_PAT_A0)              /* restored by sigreturn */
         && (g_sig_fpstate != 0)         /* fpstate pointer real  */
         && (g_sig_xmm0 == FX_PAT_A0)    /* frame held the value  */
         && ((g_sig_mxcsr & 0xFFC0u) == 0x1F80u); /* frame MXCSR
         * control bits pristine; the low 6 sticky status flags
         * legitimately record the earlier masked divide-by-zero */
    report2("sigfp", ok, (long)got);
}

/* ---- T5: execve resets the FPU state ---- */
static const char *FXSELF = "/bin/fxtest";
static const char FXMODE[] = "fxchild";

int main(int argc, char **argv)
{
    if (argc > 1 && argv[1][0] == 'f' && argv[1][1] == 'x') {
        /* fxchild: exec'd with an unmasked MXCSR left behind by
         * the parent. The reset must make this division quiet. */
        {
            uint64_t res = 0;
            __asm__ volatile(
                "movq %1, %%xmm0\n\t"
                "movq %2, %%xmm1\n\t"
                "divsd %%xmm1, %%xmm0\n\t"
                "movq %%xmm0, %0\n\t"
                : "=m"(res)
                : "m"(*(const uint64_t[]){ 0x3FF0000000000000ULL }),
                  "m"(*(const uint64_t[]){ 0ULL })
                : "xmm0", "xmm1");
            out("FXTEST exec-reset ");
            out(res == 0x7FF0000000000000ULL ? "PASS\n" : "FAIL\n");
            exit_now(res == 0x7FF0000000000000ULL ? 0 : 3);
        }
    }

    out("fxtest: M8 FPU/SSE suite\n");
    t_basic();
    t_isolation();
    t_mxcsr();
    t_sigfp();
    {
        long pid = fork_();
        if (pid == 0) {
            const char *cargv[3];
            /* Dirty the FPU: unmask all SSE exceptions. If exec
             * failed to reset the state, the child's division
             * faults with #XM and the kernel halts -- a loud,
             * unmissable failure mode. */
            __asm__ volatile("ldmxcsr %0"
                             :
                             : "m"(*(const uint32_t[]){ 0u }));
            cargv[0] = FXSELF;
            cargv[1] = FXMODE;
            cargv[2] = NULL;
            sys_call3(SYS_execve, (long)(uintptr_t)FXSELF,
                      (long)(uintptr_t)cargv, 0);
            exit_now(127);
        }
        if (pid > 0) {
            long st = 0, r = wait4_(pid, &st);
            report2("exec-reset", r == pid && (st & 0xFFFFFFFFL) == 0,
                    st & 0xFFFFFFFFL);
        } else {
            report("exec-reset", 0);
        }
    }
    out("FXTEST DONE\n");
    return g_fails == 0 ? 0 : 1;
}
