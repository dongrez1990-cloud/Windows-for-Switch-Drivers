/* SPDX-License-Identifier: MIT
 * Screen-path cost benchmark for the Switch (WoA) console - no driver involved.
 *
 * The 20 fps question ("is it the GPU or the screen path?") cannot be answered by
 * the GPU alone: today the panel is driven by Microsoft Basic Display at
 * 1024x768, so every frame a game produces is composed, rotated and scaled by
 * the CPU before it ever reaches the framebuffer. This program measures exactly
 * those CPU steps at the panel geometry so the per-frame budget is a number, not
 * a guess.
 *
 * Build (see build-screenbench.ps1): aarch64-pc-windows-msvc, /nodefaultlib,
 * Kernel32 only, own entry point - the console has no C runtime headers of use.
 *
 * Output: text on stdout (redirect to C:\SwitchWOA\screenbench.txt).
 */

typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;
typedef unsigned long long size_t;

#define NULL ((void *)0)

extern void *__stdcall VirtualAlloc(void *address, size_t bytes, uint32_t type, uint32_t protect);
extern int __stdcall VirtualFree(void *address, size_t bytes, uint32_t type);
extern int __stdcall QueryPerformanceCounter(long long *value);
extern int __stdcall QueryPerformanceFrequency(long long *value);
extern uint32_t __stdcall GetActiveProcessorCount(unsigned short group);
extern void *__stdcall GetStdHandle(int which);
extern int __stdcall WriteFile(void *handle, const void *buffer, unsigned long bytes,
                               unsigned long *written, void *overlapped);
extern void __stdcall ExitProcess(unsigned int code);
extern void __stdcall Sleep(unsigned int ms);

#define STD_OUTPUT_HANDLE (-11)
#define MEM_COMMIT 0x1000u
#define MEM_RESERVE 0x2000u
#define MEM_RELEASE 0x8000u
#define PAGE_READWRITE 0x04u

/* ---- output (no CRT) --------------------------------------------------- */
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
/* Print a per-frame cost with three decimals, scaled by 1000. */
static void out_ms_x1000(const char *label, uint64_t us_per_iter)
{
    out_text("  "); out_text(label); out_text(" = ");
    out_uint(us_per_iter / 1000u, 10, 0); out_text(".");
    out_uint(us_per_iter % 1000u, 10, 3); out_text(" ms/frame\r\n");
}

/* ---- buffers and helpers ---------------------------------------------- */
#define W_PANEL 1280              /* Switch panel native, landscape */
#define H_PANEL 720
#define W_ROT   H_PANEL           /* portrait geometry */
#define H_ROT   W_PANEL
#define W_DESK  1024              /* Microsoft Basic Display mode today */
#define H_DESK  768
#define PX       4u               /* BGRA8 */

static void *alloc(size_t bytes)
{
    void *p = VirtualAlloc(NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    return p;
}
static void release(void *p, size_t bytes) { if (p) VirtualFree(p, bytes, MEM_RELEASE); }

static void fill(void *buffer, size_t bytes, unsigned seed)
{
    unsigned *words = (unsigned *)buffer;
    size_t count = bytes / 4u, i;
    unsigned value = seed * 2654435761u + 12345u;
    for (i = 0; i < count; ++i) { value = value * 1664525u + 1013904223u; words[i] = value; }
}

static size_t copy_words(void *__restrict dst, const void *__restrict src, size_t bytes)
{
    unsigned *__restrict d = (unsigned *__restrict)dst;
    const unsigned *__restrict s = (const unsigned *__restrict)src;
    size_t words = bytes / 4u, i;
    for (i = 0; i < words; ++i) d[i] = s[i];
    return words;
}

static volatile unsigned g_sink;

/* /nodefaultlib means no CRT memcpy, but the compiler may still emit calls to it
 * (struct copies, vectorised loops), so provide a word-copy implementation
 * instead of letting the linker fail. */
void *memcpy(void *dst, const void *src, size_t bytes);
void *memcpy(void *dst, const void *src, size_t bytes)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    size_t words = bytes / 4u, i;
    for (i = 0; i < words; ++i) {
        d[i * 4u + 0u] = s[i * 4u + 0u];
        d[i * 4u + 1u] = s[i * 4u + 1u];
        d[i * 4u + 2u] = s[i * 4u + 2u];
        d[i * 4u + 3u] = s[i * 4u + 3u];
    }
    for (i = words * 4u; i < bytes; ++i) d[i] = s[i];
    return dst;
}

/* The OS's own optimised copy, for reference: a "copy costs 2.7 ms" claim is only
 * meaningful next to what production memcpy achieves on the same buffer. */
extern void __stdcall RtlMoveMemory(void *destination, const void *source, size_t bytes);

/* Threading primitives (used by the copy/rotate/blend multi-thread variants). */
extern void *__stdcall CreateThread(void *attributes, size_t stack, uint32_t (*start)(void *),
                                    void *parameter, uint32_t flags, uint32_t *id);
extern uint32_t __stdcall WaitForSingleObject(void *handle, uint32_t ms);
extern int __stdcall CloseHandle(void *handle);
#define MAX_THREADS 4u
/* ---- operations -------------------------------------------------------- */
/* 1) flat copy: the absolute floor for one 1280x720 frame. */
static unsigned op_memcpy(void *dst, const void *src, size_t bytes)
{
    return (unsigned)copy_words(dst, src, bytes);
}

/* 1b) the same copy through Kernel32. */
static unsigned op_system_copy(void *dst, const void *src, size_t bytes)
{
    RtlMoveMemory(dst, src, bytes);
    return ((const unsigned *)dst)[0];
}

/* 1c) the same copy split over 4 threads: how much headroom the frame copies
 * have when a compositor spreads them across the A57 cluster. */
struct copy_job { void *dst; const void *src; size_t begin, end; };
static uint32_t __stdcall copy_worker(void *parameter)
{
    struct copy_job *job = (struct copy_job *)parameter;
    unsigned *d = (unsigned *)((unsigned char *)job->dst + job->begin);
    const unsigned *s = (const unsigned *)((const unsigned char *)job->src + job->begin);
    size_t words = (job->end - job->begin) / 4u, i;
    for (i = 0; i < words; ++i) d[i] = s[i];
    return (uint32_t)(words & 0xffffu);
}
static unsigned op_copy_mt(void *dst, const void *src, size_t bytes, uint32_t threads)
{
    void *handles[MAX_THREADS];
    struct copy_job jobs[MAX_THREADS];
    uint32_t i;
    size_t chunk = (bytes / threads) & ~3u;
    unsigned sum = 0;
    for (i = 0; i < threads; ++i) {
        jobs[i].dst = dst; jobs[i].src = src;
        jobs[i].begin = (size_t)i * chunk;
        jobs[i].end = (i + 1u == threads) ? bytes : (size_t)(i + 1u) * chunk;
        handles[i] = CreateThread(NULL, 0, copy_worker, &jobs[i], 0, NULL);
    }
    for (i = 0; i < threads; ++i) {
        if (handles[i]) { WaitForSingleObject(handles[i], 60000); CloseHandle(handles[i]); }
        sum ^= (unsigned)jobs[i].begin;
    }
    return sum ^ ((unsigned *)dst)[0];
}

/* 2) straight pitched row copy (what a blit of the same orientation costs). */static unsigned op_rows(void *dst, const void *src, uint32_t width, uint32_t height,
                        uint32_t src_pitch, uint32_t dst_pitch)
{
    uint32_t y;
    unsigned sum = 0;
    for (y = 0; y < height; ++y) {
        sum += (unsigned)copy_words((unsigned char *)dst + y * dst_pitch,
                                    (const unsigned char *)src + y * src_pitch,
                                    (size_t)width * PX);
    }
    return sum;
}

/* 3) naive 90 degree rotation (source-order reads, destination strided). */
static unsigned op_rot90_naive(void *__restrict dst, const void *__restrict src, uint32_t w, uint32_t h)
{
    uint32_t x, y, dw = h;
    unsigned *__restrict d = (unsigned *__restrict)dst;
    const unsigned *__restrict s = (const unsigned *__restrict)src;
    unsigned sum = 0;
    for (y = 0; y < h; ++y)
        for (x = 0; x < w; ++x)
            d[(size_t)x * dw + (h - 1u - y)] = s[(size_t)y * w + x];
    sum = d[0] ^ d[(size_t)(w - 1u) * dw + (h - 1u)];
    return sum;
}

/* 4) tiled 90 degree rotation (cache-friendly, what a good software path does). */
#define TILE 32u
static unsigned op_rot90_tiled_range(void *__restrict dst, const void *__restrict src,
                                    uint32_t w, uint32_t h, uint32_t row_begin, uint32_t row_end)
{
    uint32_t bx, by, x, y, dw = h;
    unsigned *__restrict d = (unsigned *__restrict)dst;
    const unsigned *__restrict s = (const unsigned *__restrict)src;
    uint32_t sum = 0;
    for (by = row_begin; by < row_end; by += TILE)
        for (bx = 0; bx < w; bx += TILE) {
            uint32_t y_end = by + TILE < row_end ? by + TILE : row_end;
            uint32_t x_end = bx + TILE < w ? bx + TILE : w;
            for (y = by; y < y_end; ++y)
                for (x = bx; x < x_end; ++x)
                    d[(size_t)x * dw + (h - 1u - y)] = s[(size_t)y * w + x];
        }
    sum = d[(size_t)(w - 1u) * dw + (h - 1u)];
    return sum;
}
static unsigned op_rot90_tiled(void *dst, const void *src, uint32_t w, uint32_t h)
{
    uint32_t bx, by, x, y, dw = h;
    unsigned *d = (unsigned *)dst;
    const unsigned *s = (const unsigned *)src;
    for (by = 0; by < h; by += TILE)
        for (bx = 0; bx < w; bx += TILE) {
            uint32_t y_end = by + TILE < h ? by + TILE : h;
            uint32_t x_end = bx + TILE < w ? bx + TILE : w;
            for (y = by; y < y_end; ++y)
                for (x = bx; x < x_end; ++x)
                    d[(size_t)x * dw + (h - 1u - y)] = s[(size_t)y * w + x];
        }
    return d[0] ^ d[(size_t)(w - 1u) * dw + (h - 1u)];
}

/* ---- multi-threaded rotate/blend: what a compositor actually does ------ */

struct rot_job { void *dst; const void *src; uint32_t w, h, y0, y1; };
struct blend_job { void *dst; const void *top; const void *bottom; size_t first, last; };

static uint32_t __stdcall rot_worker(void *parameter)
{
    struct rot_job *job = (struct rot_job *)parameter;
    return (uint32_t)op_rot90_tiled_range(job->dst, job->src, job->w, job->h, job->y0, job->y1);
}

static uint32_t __stdcall blend_worker(void *parameter)
{
    struct blend_job *job = (struct blend_job *)parameter;
    unsigned *d = (unsigned *)job->dst;
    const unsigned *t = (const unsigned *)job->top;
    const unsigned *b = (const unsigned *)job->bottom;
    size_t i;
    for (i = job->first; i < job->last; ++i) {
        unsigned st = t[i], sb = b[i];
        unsigned a = st >> 24;
        unsigned rb, gb, bb, rt, gt, bt;
        if (a == 0u) { d[i] = sb; continue; }
        if (a == 255u) { d[i] = st | 0xff000000u; continue; }
        rb = (sb >> 16) & 0xffu; gb = (sb >> 8) & 0xffu; bb = sb & 0xffu;
        rt = (st >> 16) & 0xffu; gt = (st >> 8) & 0xffu; bt = st & 0xffu;
        rb = (rt * a + rb * (255u - a)) / 255u;
        gb = (gt * a + gb * (255u - a)) / 255u;
        bb = (bt * a + bb * (255u - a)) / 255u;
        d[i] = 0xff000000u | (rb << 16) | (gb << 8) | bb;
    }
    return (uint32_t)d[job->first];
}

static unsigned op_rot90_mt(void *dst, const void *src, uint32_t w, uint32_t h, uint32_t threads)
{
    void *handles[MAX_THREADS];
    struct rot_job jobs[MAX_THREADS];
    uint32_t i, rows = h / threads;
    unsigned sum = 0;
    for (i = 0; i < threads; ++i) {
        jobs[i].dst = dst; jobs[i].src = src; jobs[i].w = w; jobs[i].h = h;
        jobs[i].y0 = i * rows;
        jobs[i].y1 = (i + 1u == threads) ? h : (i + 1u) * rows;
        handles[i] = CreateThread(NULL, 0, rot_worker, &jobs[i], 0, NULL);
    }
    for (i = 0; i < threads; ++i) {
        if (handles[i]) { WaitForSingleObject(handles[i], 60000); CloseHandle(handles[i]); }
        sum ^= (unsigned)jobs[i].y0;
    }
    return sum ^ ((unsigned *)dst)[0];
}

static unsigned op_blend_mt(void *dst, const void *top, const void *bottom, size_t pixels,
                            uint32_t threads)
{
    void *handles[MAX_THREADS];
    struct blend_job jobs[MAX_THREADS];
    uint32_t i;
    size_t chunk = pixels / threads;
    unsigned sum = 0;
    for (i = 0; i < threads; ++i) {
        jobs[i].dst = dst; jobs[i].top = top; jobs[i].bottom = bottom;
        jobs[i].first = (size_t)i * chunk;
        jobs[i].last = (i + 1u == threads) ? pixels : (size_t)(i + 1u) * chunk;
        handles[i] = CreateThread(NULL, 0, blend_worker, &jobs[i], 0, NULL);
    }
    for (i = 0; i < threads; ++i) {
        if (handles[i]) { WaitForSingleObject(handles[i], 60000); CloseHandle(handles[i]); }
        sum ^= (unsigned)jobs[i].first;
    }
    return sum ^ ((unsigned *)dst)[0];
}

/* 4b) rotation with hoisted pointers: contiguous destination runs of TILE
 * pixels (= one 64-byte line at 4 bytes/pixel) and strided source reads that
 * stay inside the tile. This is what the current software path should be doing;
 * the per-pixel `x * dw + (h-1-y)` multiply in the versions above is the reason
 * they look 5x slower than the silicon can do. */
static unsigned op_rot90_fast_range(void *__restrict dst, const void *__restrict src,
                                    uint32_t w, uint32_t h, uint32_t x_begin, uint32_t x_end)
{
    unsigned *__restrict d = (unsigned *__restrict)dst;
    const unsigned *__restrict s = (const unsigned *__restrict)src;
    uint32_t x, y, y0, y1;
    for (y0 = 0; y0 < h; y0 += TILE) {
        y1 = y0 + TILE < h ? y0 + TILE : h;
        for (x = x_begin; x < x_end; ++x) {
            /* Destination columns run from (h-1-y0) downwards: fill backwards
             * so the mapping matches tg_present_rot90() exactly. */
            unsigned *__restrict drow = d + (size_t)x * h + (h - 1u - y0);
            const unsigned *scol = s + (size_t)y0 * w + x;
            for (y = y0; y < y1; ++y) {
                *drow = *scol;
                --drow;
                scol += w;
            }
        }
    }
    return d[(size_t)(x_begin) * h];
}
static unsigned op_rot90_fast(void *dst, const void *src, uint32_t w, uint32_t h)
{
    return op_rot90_fast_range(dst, src, w, h, 0, w);
}
struct xrange_job { void *dst; const void *src; uint32_t w, h, x0, x1; };

static uint32_t __stdcall rot_fast_worker(void *parameter)
{
    struct xrange_job *job = (struct xrange_job *)parameter;
    return (uint32_t)op_rot90_fast_range(job->dst, job->src, job->w, job->h, job->x0, job->x1);
}

static unsigned op_rot90_fast_mt(void *dst, const void *src, uint32_t w, uint32_t h,
                                 uint32_t threads)
{
    void *handles[MAX_THREADS];
    struct xrange_job jobs[MAX_THREADS];
    uint32_t i, chunk = w / threads;
    unsigned sum = 0;
    for (i = 0; i < threads; ++i) {
        jobs[i].dst = dst; jobs[i].src = src; jobs[i].w = w; jobs[i].h = h;
        jobs[i].x0 = i * chunk;
        jobs[i].x1 = (i + 1u == threads) ? w : (i + 1u) * chunk;
        handles[i] = CreateThread(NULL, 0, rot_fast_worker, &jobs[i], 0, NULL);
    }
    for (i = 0; i < threads; ++i) {
        if (handles[i]) { WaitForSingleObject(handles[i], 60000); CloseHandle(handles[i]); }
        sum ^= (unsigned)jobs[i].x0;
    }
    return sum ^ ((unsigned *)dst)[0];
}

/* 5) nearest-neighbour scale with an optional 90 degree rotation in one pass. */

static unsigned op_scale(void *dst, const void *src, uint32_t sw, uint32_t sh,
                         uint32_t dw, uint32_t dh, int rotate)
{
    uint32_t x, y;
    unsigned *d = (unsigned *)dst;
    const unsigned *s = (const unsigned *)src;
    for (y = 0; y < dh; ++y) {
        uint32_t sy = (uint32_t)(((uint64_t)y * sh) / dh);
        for (x = 0; x < dw; ++x) {
            uint32_t sx = (uint32_t)(((uint64_t)x * sw) / dw);
            unsigned pixel = s[(size_t)sy * sw + sx];
            if (rotate) d[(size_t)(dw - 1u - x) * dh + y] = pixel;
            else d[(size_t)y * dw + x] = pixel;
        }
    }
    return d[0] ^ d[(size_t)(dw - 1u) * dh];
}

/* 5b) scale + 90 degree rotation with contiguous destination runs: the fused
 * form a present path actually wants, done with hoisted pointers instead of a
 * per-pixel multiply and division. */
static unsigned op_scale_rot_fast(void *__restrict dst, const void *__restrict src,
                                  uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh)
{
    unsigned *__restrict d = (unsigned *__restrict)dst;
    const unsigned *__restrict s = (const unsigned *__restrict)src;
    uint32_t x, y;
    for (x = 0; x < dw; ++x) {
        uint32_t sx = (uint32_t)(((uint64_t)x * sw) / dw);
        unsigned *__restrict drow = d + (size_t)(dw - 1u - x) * dh;
        for (y = 0; y < dh; ++y) {
            uint32_t sy = (uint32_t)(((uint64_t)y * sh) / dh);
            *drow++ = s[(size_t)sy * sw + sx];
        }
    }
    return d[0];
}

/* 6) DWM-style SrcOver blend of two 720p buffers (alpha from the top byte). */
static unsigned op_blend(void *dst, const void *top, const void *bottom, size_t pixels)
{
    unsigned *d = (unsigned *)dst;
    const unsigned *t = (const unsigned *)top;
    const unsigned *b = (const unsigned *)bottom;
    size_t i;
    for (i = 0; i < pixels; ++i) {
        unsigned st = t[i], sb = b[i];
        unsigned a = st >> 24;
        unsigned rb, gb, bb, rt, gt, bt;
        if (a == 0u) { d[i] = sb; continue; }
        if (a == 255u) { d[i] = st | 0xff000000u; continue; }
        rb = (sb >> 16) & 0xffu; gb = (sb >> 8) & 0xffu; bb = sb & 0xffu;
        rt = (st >> 16) & 0xffu; gt = (st >> 8) & 0xffu; bt = st & 0xffu;
        rb = (rt * a + rb * (255u - a)) / 255u;
        gb = (gt * a + gb * (255u - a)) / 255u;
        bb = (bt * a + bb * (255u - a)) / 255u;
        d[i] = 0xff000000u | (rb << 16) | (gb << 8) | bb;
    }
    return d[0];
}

/* ---- timing harness ---------------------------------------------------- */
static long long g_freq;

static uint64_t now_us(void)
{
    long long counter = 0;
    QueryPerformanceCounter(&counter);
    return (uint64_t)((counter * 1000000ll) / g_freq);
}

/* Runs fn `iters` times and reports the average microseconds per iteration. */
#define MEASURE(label, iters, expr)                                  \
    do {                                                             \
        uint64_t t0_, t1_, i_;                                       \
        unsigned sink_ = 0;                                          \
        t0_ = now_us();                                              \
        for (i_ = 0; i_ < (uint64_t)(iters); ++i_) { sink_ ^= (expr); } \
        t1_ = now_us();                                              \
        g_sink = sink_;                                              \
        out_text("  "); out_text(label); out_text(" : ");            \
        out_uint((t1_ - t0_) / (uint64_t)(iters), 10, 0); out_text(" us/frame"); \
        out_text(" (x"); out_uint((uint64_t)(iters), 10, 0); out_text(")\r\n"); \
    } while (0)

/* ---- main ------------------------------------------------------------- */
void main_entry(void)
{
    const size_t frame_bytes = (size_t)W_PANEL * H_PANEL * PX;
    void *src, *dst, *dst2, *top, *bottom;
    uint64_t t0, t1;

    QueryPerformanceFrequency(&g_freq);
    out_text("=== Switch screen-path CPU benchmark ===\r\n");
    out_field("qpc_hz", (uint64_t)g_freq, 10);
    out_field("cores", GetActiveProcessorCount(0xFFFF), 10);
    out_field("frame_bytes", (uint64_t)frame_bytes, 10);

    src = alloc(frame_bytes);
    dst = alloc(frame_bytes);
    dst2 = alloc(frame_bytes);
    top = alloc(frame_bytes);
    bottom = alloc(frame_bytes);
    if (!src || !dst || !dst2 || !top || !bottom) {
        out_text("allocation failed\r\n");
        ExitProcess(2);
    }
    fill(src, frame_bytes, 1);
    fill(top, frame_bytes, 2);
    fill(bottom, frame_bytes, 3);

    /* Warm the caches and settle the CPU governor before the real runs. */
    {
        int i;
        for (i = 0; i < 8; ++i) g_sink ^= op_memcpy(dst, src, frame_bytes);
        Sleep(200);
    }

    out_text("\r\n--- one 1280x720 BGRA frame (3.6 MB) ---\r\n");
    MEASURE("flat copy", 40, op_memcpy(dst, src, frame_bytes));
    MEASURE("pitched row copy", 40, op_rows(dst, src, W_PANEL, H_PANEL, W_PANEL * PX, W_PANEL * PX));
    MEASURE("rot90 naive 720x1280->1280x720", 20, op_rot90_naive(dst, src, W_PANEL, H_PANEL));
    MEASURE("rot90 tiled 720x1280->1280x720", 20, op_rot90_tiled(dst, src, W_PANEL, H_PANEL));
    MEASURE("rot90 fast (hoisted ptrs) 1 thread", 20, op_rot90_fast(dst, src, W_PANEL, H_PANEL));
    MEASURE("scale 1280x720 -> 1024x768", 20, op_scale(dst2, src, W_PANEL, H_PANEL, W_DESK, H_DESK, 0));
    MEASURE("scale+rot90 1280x720 -> 768x1024", 20, op_scale(dst2, src, W_PANEL, H_PANEL, H_DESK, W_DESK, 1));
    MEASURE("scale+rot90 fast 1280x720 -> 768x1024", 20, op_scale_rot_fast(dst2, src, W_PANEL, H_PANEL, H_DESK, W_DESK));
    MEASURE("SrcOver blend 720p", 20, op_blend(dst, top, bottom, (size_t)W_PANEL * H_PANEL));

    out_text("\r\n--- the same two dominant steps with 4 threads (compositor) ---\r\n");
    MEASURE("rot90 tiled x4 threads", 20, op_rot90_mt(dst, src, W_PANEL, H_PANEL, 4));
    MEASURE("rot90 fast x4 threads", 20, op_rot90_fast_mt(dst, src, W_PANEL, H_PANEL, 4));
    MEASURE("SrcOver blend x4 threads", 20, op_blend_mt(dst, top, bottom, (size_t)W_PANEL * H_PANEL, 4));
    MEASURE("flat copy via RtlMoveMemory", 40, op_system_copy(dst, src, frame_bytes));
    MEASURE("flat copy x4 threads", 40, op_copy_mt(dst, src, frame_bytes, 4));

    /* Cache-resident copy: separates "core is slow" from "DRAM is slow". */
    {
        size_t small = 512u * 1024u;
        void *s2 = alloc(small);
        void *d3 = alloc(small);
        if (s2 && d3) {
            fill(s2, small, 7);
            out_text("\r\n--- cache-resident copy (512 KB) ---\r\n");
            MEASURE("512 KB flat copy", 200, op_memcpy(d3, s2, small));
            release(s2, small);
            release(d3, small);
        }
    }

    /* Bandwidth of the flat copy, for the report. */
    {
        uint64_t best = ~0ull;
        int i;
        for (i = 0; i < 15; ++i) {
            t0 = now_us();
            g_sink ^= op_memcpy(dst, src, frame_bytes);
            t1 = now_us();
            if (t1 - t0 < best) best = t1 - t0;
        }
        out_text("\r\n--- copy bandwidth ---\r\n");
        out_field("best_copy_us", best, 10);
        /* bytes/s = bytes / (us/1e6); report MB/s = bytes * 1e6 / (us * 1e6) * 1e6 ... */
        out_field("copy_MB_per_s", best ? (uint64_t)((frame_bytes * 1000000ull) / (best * 1024ull * 1024ull)) : 0, 10);
    }

    /* The composed cost of one software-presented frame at the portrait
     * geometry the games actually show: rotate the 720p render target, scale it
     * to the desktop mode, and copy it to the framebuffer. */
    {
        uint32_t rot_us, scale_us, copy_us;
        uint64_t t;
        int i;
        uint64_t acc = 0;

        t = now_us();
        for (i = 0; i < 10; ++i) g_sink ^= op_rot90_fast(dst, src, W_PANEL, H_PANEL);
        rot_us = (uint32_t)((now_us() - t) / 10u);

        t = now_us();
        for (i = 0; i < 10; ++i) g_sink ^= op_scale_rot_fast(dst2, src, W_PANEL, H_PANEL, H_DESK, W_DESK);
        scale_us = (uint32_t)((now_us() - t) / 10u);

        t = now_us();
        for (i = 0; i < 10; ++i) g_sink ^= op_memcpy(dst, src, frame_bytes);
        copy_us = (uint32_t)((now_us() - t) / 10u);

        acc = (uint64_t)rot_us + scale_us + copy_us;
        out_text("\r\n--- software present budget, 1 thread (rotate + scale+rot + copy) ---\r\n");
        out_field("rotate_us", rot_us, 10);
        out_field("scale_us", scale_us, 10);
        out_field("copy_us", copy_us, 10);
        out_field("total_us", acc, 10);
        out_field("max_fps_this_part", acc ? (1000000ull / acc) : 0, 10);
        out_ms_x1000("total_ms_x1000", acc);
    }

    out_field("\r\nsink", g_sink, 16);
    release(src, frame_bytes);
    release(dst, frame_bytes);
    release(dst2, frame_bytes);
    release(top, frame_bytes);
    release(bottom, frame_bytes);
    ExitProcess(0);
}
