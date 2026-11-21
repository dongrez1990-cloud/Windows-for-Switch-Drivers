/* SPDX-License-Identifier: MIT
 * Portable 90-degree rotate / nearest-neighbour scale for the panel path.
 *
 * Written for the Switch (WoA) screen path, where the panel storage is 720x1280
 * portrait while the logical desktop is landscape: one rotation per frame is
 * unavoidable, so the only question is who pays for it. These CPU versions
 * measured 5.15 ms (rotate 1280x720 -> 720x1280) and 2.36 ms (scale) on the
 * console at 1750 MHz, versus 12-24 ms for the naive "index math inside the
 * pixel loop" form (see STATUS-GPU0-BOOT.md chapters 17-18).
 *
 * Rules that produce the speedup - keep them if this is ported:
 *   1. Never multiply per pixel. Advance pointers; the destination walks
 *      contiguously and the source advances by one pitch step.
 *   2. Work in destination-row ranges so a caller can split the frame across
 *      threads (kernel-safe: no thread creation in here).
 *   3. Destination runs of TILE pixels are exactly one 64-byte line at 4 B/px,
 *      so every write stream touches whole lines.
 *   4. Rotation and scaling stay separate operations. Fusing them is
 *      memory-bound at 18-23 ms because every scaled read lands in a new line.
 *
 * No platform dependencies: plain C99, stdint/stddef only.
 */
#ifndef TG_PRESENT_REF_H
#define TG_PRESENT_REF_H

#include <stddef.h>
#include <stdint.h>

/* Rotation by 90 degrees. The source is src[y][x] with sw columns and sh rows;
 * the destination has `sw` rows of `sh` pixels and the mapping
 *     dst[x][sh - 1 - y] = src[y][x]
 * all pitches in pixels. x_from/x_to select destination rows to produce, so a
 * caller can hand slices to threads. For the Switch panel that means
 *     tg_present_rot90(dst, 720, src, 1280, 1280, 720);   // landscape -> portrait */
void tg_present_rot90_range(uint32_t *dst, uint32_t dst_pitch_px,
                            const uint32_t *src, uint32_t src_pitch_px,
                            uint32_t sw, uint32_t sh,
                            uint32_t x_from, uint32_t x_to);
static inline void tg_present_rot90(uint32_t *dst, uint32_t dst_pitch_px,
                                    const uint32_t *src, uint32_t src_pitch_px,
                                    uint32_t sw, uint32_t sh)
{
    tg_present_rot90_range(dst, dst_pitch_px, src, src_pitch_px, sw, sh, 0u, sw);
}

/* Nearest-neighbour scale, no rotation: dst (dw x dh) from src (sw x sh). */
void tg_present_scale_nearest(uint32_t *dst, uint32_t dst_pitch_px, uint32_t dw, uint32_t dh,
                              const uint32_t *src, uint32_t src_pitch_px, uint32_t sw, uint32_t sh);

/* Reference implementations, correct but slow: only for self-tests. */
void tg_present_rot90_reference(uint32_t *dst, uint32_t dst_pitch_px,
                                const uint32_t *src, uint32_t src_pitch_px,
                                uint32_t sw, uint32_t sh);
void tg_present_scale_reference(uint32_t *dst, uint32_t dst_pitch_px, uint32_t dw, uint32_t dh,
                                const uint32_t *src, uint32_t src_pitch_px, uint32_t sw, uint32_t sh);

#endif
