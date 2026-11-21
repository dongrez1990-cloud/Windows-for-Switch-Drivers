/* SPDX-License-Identifier: MIT
 * T210 GM20B OPP helpers: CVB voltage (retail FIXED table) and rounding to
 * the MAX77621 rail alignment (offset 606.25 mV, step 6.25 mV).
 */
#include "t210_opp.h"

#define SPEEDO_SCALE   100
#define RAIL_OFFSET_UV 606250u  /* 606.25 mV in uV */
#define RAIL_STEP_UV   6250u    /* 6.25 mV in uV */
#define VMIN_UV        800000u  /* Switchroot default VTS floor (t214 policy) */
#define PLL_MIN_UV     918000u  /* gpu_cvb_dvfs_table speedo 4: PLL-start check only */
#define MAX_UV         1113000u /* max_mv 1113 */

static int32_t div_nearest(int32_t n, int32_t d)
{
    return n < 0 ? -((-n + d / 2) / d) : (n + d / 2) / d;
}

uint32_t t210_gpu_freq_khz(uint32_t index)
{
    return index < T210_GPU_OPP_COUNT ? t210_gpu_cvb[index].khz : 0u;
}

uint32_t t210_gpu_index(uint32_t khz)
{
    uint32_t i;
    for (i = 0; i < T210_GPU_OPP_COUNT; ++i)
        if (t210_gpu_cvb[i].khz == khz) return i;
    return T210_GPU_OPP_COUNT;
}

/* CVB voltage in uV for one OPP row, rounded up to the MAX77621 rail.
 * The cvb.c result is already in uV (voltage_scale=1000 of an mV value).
 * Returns 0 for unknown rows/frequency or when the envelope exceeds max_mv.
 * Note: per-speedo table selection (gpu_cvb_dvfs_table rows) is a later step;
 * this function models the retail FIXED_FREQ_CVB_TABLE path. */
uint32_t t210_gpu_cvb_uv(uint32_t speedo, uint32_t khz)
{
    const struct t210_gpu_cvb *row;
    uint32_t idx = t210_gpu_index(khz);
    int32_t base, uv;
    if (!speedo || speedo > 5000u || idx == T210_GPU_OPP_COUNT) return 0u;
    row = &t210_gpu_cvb[idx];
    base = div_nearest(row->c2 * (int32_t)speedo, SPEEDO_SCALE);
    base = div_nearest((base + row->c1) * (int32_t)speedo, SPEEDO_SCALE);
    uv = base + row->c0;               /* uV */
    if (uv < (int32_t)VMIN_UV) uv = VMIN_UV;
    if (uv > (int32_t)MAX_UV) return 0u;
    {
        int64_t rem = (int64_t)uv - RAIL_OFFSET_UV;
        int64_t steps;
        if (rem < 0) rem = 0;
        steps = (rem + RAIL_STEP_UV - 1) / RAIL_STEP_UV;
        return (uint32_t)(steps * RAIL_STEP_UV + RAIL_OFFSET_UV);
    }
}


