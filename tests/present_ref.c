/* SPDX-License-Identifier: MIT
 * Implementation + self-test of the portable panel-path rotate/scale helpers.
 *
 * Build and run with tools\run-host-tests.ps1 (x64, /nodefaultlib, Kernel32 only).
 * The test verifies every destination pixel against a reference implementation
 * for the panel geometry, for an odd geometry with padded pitches, and for a
 * frame split into four row ranges (thread slicing).
 */
#include "present_ref.h"

#define TILE 32u

/* ---- fast paths -------------------------------------------------------- */
void tg_present_rot90_range(uint32_t *dst, uint32_t dst_pitch_px,
                            const uint32_t *src, uint32_t src_pitch_px,
                            uint32_t sw, uint32_t sh,
                            uint32_t x_from, uint32_t x_to)
{
    uint32_t x, y, y0, y1;
    if (x_to > sw) x_to = sw;
    for (y0 = 0; y0 < sh; y0 += TILE) {
        /* Source rows [y0,y1) map to destination columns (sh-1-y0) down to
         * (sh-y1): one contiguous run, filled from the high end downwards. */
        y1 = y0 + TILE < sh ? y0 + TILE : sh;
        for (x = x_from; x < x_to; ++x) {
            uint32_t *d = dst + (size_t)x * dst_pitch_px + (sh - 1u - y0);
            const uint32_t *s = src + (size_t)y0 * src_pitch_px + x;
            for (y = y0; y < y1; ++y) {
                *d = *s;
                --d;
                s += src_pitch_px;
            }
        }
    }
}

void tg_present_scale_nearest(uint32_t *dst, uint32_t dst_pitch_px, uint32_t dw, uint32_t dh,
                              const uint32_t *src, uint32_t src_pitch_px, uint32_t sw, uint32_t sh)
{
    uint32_t x, y;
    for (y = 0; y < dh; ++y) {
        uint32_t sy = (uint32_t)(((uint64_t)y * sh) / dh);
        const uint32_t *s = src + (size_t)sy * src_pitch_px;
        uint32_t *d = dst + (size_t)y * dst_pitch_px;
        for (x = 0; x < dw; ++x) {
            uint32_t sx = (uint32_t)(((uint64_t)x * sw) / dw);
            *d++ = s[sx];
        }
    }
}

/* ---- reference paths (same mapping, deliberately naive) ---------------- */
void tg_present_rot90_reference(uint32_t *dst, uint32_t dst_pitch_px,
                                const uint32_t *src, uint32_t src_pitch_px,
                                uint32_t sw, uint32_t sh)
{
    uint32_t x, y;
    for (y = 0; y < sh; ++y)
        for (x = 0; x < sw; ++x)
            dst[(size_t)x * dst_pitch_px + (sh - 1u - y)] = src[(size_t)y * src_pitch_px + x];
}

void tg_present_scale_reference(uint32_t *dst, uint32_t dst_pitch_px, uint32_t dw, uint32_t dh,
                                const uint32_t *src, uint32_t src_pitch_px, uint32_t sw, uint32_t sh)
{
    uint32_t x, y;
    for (y = 0; y < dh; ++y)
        for (x = 0; x < dw; ++x)
            dst[(size_t)y * dst_pitch_px + x] =
                src[(size_t)(((uint64_t)y * sh) / dh) * src_pitch_px + (uint32_t)(((uint64_t)x * sw) / dw)];
}

/* ---- self-test --------------------------------------------------------- */
/* This dev box has clang + SDK headers but no MSVC vcruntime, so the few
 * services used here are declared directly (as in test_clock_t210_host.c). */
extern void *__stdcall GetStdHandle(int which);
extern int __stdcall WriteFile(void *handle, const void *buffer, unsigned long bytes,
                               unsigned long *written, void *overlapped);
extern void __stdcall ExitProcess(unsigned int code);
extern int __stdcall QueryPerformanceCounter(long long *value);
extern int __stdcall QueryPerformanceFrequency(long long *value);

#define STD_OUTPUT_HANDLE (-11)

void *memset(void *dst, int value, size_t bytes)
{
    unsigned char *p = (unsigned char *)dst;
    while (bytes--) *p++ = (unsigned char)value;
    return dst;
}

static void out_text(const char *text)
{
    unsigned long written = 0, length = 0;
    while (text[length]) ++length;
    if (length) WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, length, &written, NULL);
}
static void out_uint(uint64_t value)
{
    char buffer[24];
    int length = 0;
    do { buffer[length++] = (char)('0' + (value % 10u)); value /= 10u; } while (value && length < 23);
    while (length) { char c[2]; c[0] = buffer[--length]; c[1] = 0; out_text(c); }
}
static void out_line(const char *text) { out_text(text); out_text("\r\n"); }

#define SRC_W 1280u
#define SRC_H 720u
#define SRC_PITCH SRC_W
#define DST_PITCH SRC_H
#define BUF_WORDS (SRC_W * SRC_H)

static uint32_t g_src[BUF_WORDS + 64];
static uint32_t g_dst[BUF_WORDS + 64];
static uint32_t g_ref[BUF_WORDS + 64];
static uint32_t g_small[256 * 256];

/* Compare rows x cols of two buffers with the given pitch. */
static int same(const char *label, const uint32_t *a, const uint32_t *b,
                uint32_t pitch, uint32_t rows, uint32_t cols)
{
    uint32_t r, c;
    for (r = 0; r < rows; ++r)
        for (c = 0; c < cols; ++c) {
            uint32_t want = b[(size_t)r * pitch + c];
            uint32_t got = a[(size_t)r * pitch + c];
            if (want != got) {
                out_text("MISMATCH "); out_text(label);
                out_text(" dst row "); out_uint(r);
                out_text(" col "); out_uint(c);
                out_line("");
                return 0;
            }
        }
    return 1;
}

int run_tests(void)
{
    uint64_t i;
    long long freq = 0, t0, t1;

    for (i = 0; i < BUF_WORDS; ++i)
        g_src[i] = (uint32_t)(i * 2654435761u) ^ (uint32_t)(i >> 3);

    /* 1) panel geometry: 1280x720 landscape -> 1280 rows x 720 columns. */
    tg_present_rot90_reference(g_ref, DST_PITCH, g_src, SRC_PITCH, SRC_W, SRC_H);
    tg_present_rot90(g_dst, DST_PITCH, g_src, SRC_PITCH, SRC_W, SRC_H);
    if (!same("rot90 1280x720", g_dst, g_ref, DST_PITCH, SRC_W, SRC_H)) return 1;
    out_line("rotate 1280x720 -> 720x1280 storage: pixel-exact vs reference");

    /* 2) odd geometry with padded source and destination pitches. */
    {
        const uint32_t sw = 93, sh = 57, spitch = 96, dpitch = 62;
        uint32_t x, y;
        for (i = 0; i < 256 * 256; ++i) g_small[i] = 0;
        for (y = 0; y < sh; ++y)
            for (x = 0; x < sw; ++x)
                g_small[(size_t)y * spitch + x] = (y << 16) | x;
        tg_present_rot90_reference(g_ref, dpitch, g_small, spitch, sw, sh);
        tg_present_rot90(g_dst, dpitch, g_small, spitch, sw, sh);
        if (!same("rot90 93x57 padded", g_dst, g_ref, dpitch, sw, sh)) return 1;
        out_line("rotate 93x57 with padding: pixel-exact vs reference");
    }

    /* 3) four row ranges must sum to the whole frame (thread slicing). */
    {
        uint32_t slice;
        /* Test 2 overwrote g_ref with the small padded frame: rebuild it. */
        tg_present_rot90_reference(g_ref, DST_PITCH, g_src, SRC_PITCH, SRC_W, SRC_H);
        for (i = 0; i < BUF_WORDS; ++i) g_dst[i] = 0;
        for (slice = 0; slice < 4u; ++slice) {
            uint32_t from = (uint32_t)((size_t)slice * SRC_W / 4u);
            uint32_t to = (uint32_t)(((size_t)slice + 1u) * SRC_W / 4u);
            tg_present_rot90_range(g_dst, DST_PITCH, g_src, SRC_PITCH, SRC_W, SRC_H, from, to);
        }
        if (!same("rot90 4 slices", g_dst, g_ref, DST_PITCH, SRC_W, SRC_H)) return 1;
        out_line("rotate split in 4 row ranges: identical to one-shot");
    }

    /* 4) scale 1280x720 -> 1024x768. */
    tg_present_scale_reference(g_ref, 1024, 1024, 768, g_src, SRC_PITCH, SRC_W, SRC_H);
    tg_present_scale_nearest(g_dst, 1024, 1024, 768, g_src, SRC_PITCH, SRC_W, SRC_H);
    if (!same("scale 1024x768", g_dst, g_ref, 1024, 768, 1024)) return 1;
    out_line("scale 1280x720 -> 1024x768: pixel-exact vs reference");

    /* 5) timing on this host, for reference only. */
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    for (i = 0; i < 20; ++i) tg_present_rot90(g_dst, DST_PITCH, g_src, SRC_PITCH, SRC_W, SRC_H);
    QueryPerformanceCounter(&t1);
    out_text("  rot90 one frame (host): ");
    out_uint((uint64_t)(((t1 - t0) * 1000000ll) / freq / 20ll));
    out_line(" us");

    out_line("present_ref self-test passed.");
    return 0;
}

/* /nodefaultlib: our own entry point. */
void host_main(void) { ExitProcess((unsigned int)run_tests()); }
