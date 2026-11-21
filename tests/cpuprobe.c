/* SPDX-License-Identifier: MIT
 * CPU clock probe for the Switch (WoA) console - no driver involved.
 *
 * Why this matters: the screen-path benchmark (screenbench.c) produced
 * 12.8-13.7 ms for one tiled 90 degree rotation of a 720p frame. Whether that is
 * "the A57 cluster is too slow" or "the cluster is running at the retail
 * 1024 MHz point instead of 1786 MHz" changes the conclusion by ~1.75x, and the
 * answer decides whether the next step is a code change or a clock change.
 *
 * Method: QueryThreadCycleTime reports the thread's own cycle counter, so
 * cycles/wall-second is the core frequency while the thread is running. The
 * same call is repeated around a Sleep() window: a core-cycle counter does NOT
 * advance while the thread is asleep, a wall-clock-derived counter does. That
 * distinguishes the two without trusting any firmware table.
 *
 * Build: tools\build-screenbench.ps1 (same recipe: aarch64, /nodefaultlib,
 * Kernel32 only, own entry point).
 */

typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;

#define NULL ((void *)0)

extern void *__stdcall GetCurrentThread(void);
extern int __stdcall QueryThreadCycleTime(void *thread, uint64_t *cycles);
extern int __stdcall QueryPerformanceCounter(long long *value);
extern int __stdcall QueryPerformanceFrequency(long long *value);
extern int __stdcall GetActiveProcessorCount(unsigned short group);
extern void *__stdcall GetStdHandle(int which);
extern int __stdcall WriteFile(void *handle, const void *buffer, unsigned long bytes,
                               unsigned long *written, void *overlapped);
extern void __stdcall ExitProcess(unsigned int code);
extern void __stdcall Sleep(unsigned int ms);

#define STD_OUTPUT_HANDLE (-11)

static long long g_freq;

static void out_text(const char *text)
{
    unsigned long written = 0;
    unsigned long length = 0;
    while (text[length]) ++length;
    if (length) WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, length, &written, NULL);
}
static void out_uint(uint64_t value, unsigned base, int pad_to)
{
    char buffer[24];
    const char *digits = "0123456789ABCDEF";
    int length = 0;
    do { buffer[length++] = digits[value % base]; value /= base; } while (value && length < 23);
    while (pad_to > length) { out_text("0"); --pad_to; }
    while (length) { char c[2]; c[0] = buffer[--length]; c[1] = 0; out_text(c); }
}
static void out_field(const char *label, uint64_t value, unsigned base)
{
    out_text("  "); out_text(label); out_text(" = ");
    out_uint(value, base, 0); out_text("\r\n");
}

static uint64_t cycles_now(void)
{
    uint64_t cycles = 0;
    if (!QueryThreadCycleTime(GetCurrentThread(), &cycles)) return 0;
    return cycles;
}
static uint64_t qpc_now(void)
{
    long long counter = 0;
    QueryPerformanceCounter(&counter);
    return (uint64_t)counter;
}

/* Busy work that the compiler cannot fold away: a dependent integer chain.
 * The value only depends on itself, so the loop is a serial dependency and the
 * cost per iteration is fixed by the pipeline, not by memory. */
static uint32_t g_seed = 3;

static uint32_t busy(uint64_t iterations)
{
    uint32_t x = g_seed;
    uint64_t i;
    for (i = 0; i < iterations; ++i) {
        x = x * 5u + 1u;
        x ^= x >> 7;
    }
    g_seed = x;
    return x;
}

void main_entry(void)
{
    uint64_t c0, c1, q0, q1;
    uint64_t khz;
    int round;

    QueryPerformanceFrequency(&g_freq);
    out_text("=== CPU clock probe ===\r\n");
    out_field("qpc_hz", (uint64_t)g_freq, 10);
    out_field("cores", GetActiveProcessorCount(0xFFFF), 10);

    /* Sanity: does the cycle counter advance at all? */
    c0 = cycles_now();
    Sleep(50);
    c1 = cycles_now();
    out_text("\r\n--- cycle counter sanity (50 ms sleep) ---\r\n");
    out_field("cycles_during_sleep", c1 - c0, 10);

    out_text("\r\n--- core clock while busy (3 rounds, 250 ms each) ---\r\n");
    khz = 0;
    for (round = 0; round < 3; ++round) {
        uint64_t target_ticks = (uint64_t)g_freq / 4u;   /* ~250 ms */
        uint64_t begin = qpc_now();
        uint64_t cbegin, cend, qend;
        cbegin = cycles_now();
        q0 = begin;
        while (qpc_now() - begin < target_ticks) busy(20000u);
        cend = cycles_now();
        qend = qpc_now();
        c0 = cbegin; c1 = cend; q1 = qend;
        if (q1 != q0) {
            khz = ((c1 - c0) * (uint64_t)g_freq) / ((q1 - q0) * 1000ull);
            out_text("  round "); out_uint((uint64_t)(round + 1), 10, 0);
            out_text(": cycles="); out_uint(c1 - c0, 10, 0);
            out_text(" wall_us="); out_uint(((q1 - q0) * 1000000ull) / (uint64_t)g_freq, 10, 0);
            out_text(" -> clock_khz="); out_uint(khz, 10, 0); out_text("\r\n");
        } else {
            out_text("  round: no wall time elapsed, unusable\r\n");
        }
    }

    out_text("\r\n--- summary ---\r\n");
    out_field("core_clock_khz", khz, 10);
    out_field("core_clock_mhz", khz / 1000u, 10);
    /* The two retail operating points: 1024 MHz (handheld default) and
     * 1786 MHz (boost/docked). Report which one this boot is closest to. */
    if (khz) {
        out_text("  nearest point: ");
        if (khz < 1300000ull) out_text("about 1024 MHz (retail handheld)");
        else out_text("about 1786 MHz (boost/docked)");
        out_text("\r\n");
    }

    ExitProcess(0);
}
