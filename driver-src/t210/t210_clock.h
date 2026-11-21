/* SPDX-License-Identifier: MIT
 * T210 (Erista) GM20B clock layer ??port of tegra-x1-wddm
 * platform/t210_clock.[ch] for T210. GM20B GPCPLL registers are the same
 * block inside the GPU aperture; the deltas are the Erista board check
 * (done by t210_power), the T210 CVB tables (t210_opp) and the MAX77621
 * 0x1C rail instead of MAX77812 M1.
 */
#ifndef TEGRA_GPU_T210_CLOCK_H
#define TEGRA_GPU_T210_CLOCK_H

#include "t210_power.h"

/* Fixed bring-up profiles, not a governor. */
#define TG_GPU_FIXED_KHZ        460800u  /* retail safe step */
#define TG_GPU_LOW_POWER_KHZ    307200u
#define TG_GPU_HIGH_KHZ         844800u  /* retail default; DTS rework allows 921600 */
#define TG_GPU_REFERENCE_KHZ    38400u
#define TG_GPU_CLOCK_SAVED_REGS 13u

enum tg_gpu_clock_result {
    TG_GPU_CLOCK_OK, TG_GPU_CLOCK_BAD_STATE, TG_GPU_CLOCK_RAIL_ERROR,
    TG_GPU_CLOCK_CALIBRATION_ERROR, TG_GPU_CLOCK_READBACK_ERROR,
    TG_GPU_CLOCK_COUNTER_ERROR
};

struct tg_gpu_clock {
    bool owned;
    bool running;
    uint32_t saved[TG_GPU_CLOCK_SAVED_REGS];
    uint32_t measured_khz;
    uint32_t target_khz;
    uint32_t rail_uv;
    uint32_t counter;
    uint32_t stage;
    int32_t adc_slope_uv;
    int32_t adc_intercept_uv;
    int32_t external_calibration;
    uint32_t gpu_speedo;
};

/* Requires an idle GPU just acquired by tg_gpu_power_up and exclusive
 * register ownership. The rail is set before PLL startup; power_down restores
 * it after the clock is stopped. CPU/DDR are untouched. */
enum tg_gpu_clock_result tg_gpu_clock_start(const struct tg_gpu_power_io *io,
    const struct tg_gpu_power *power, uint32_t target_khz, struct tg_gpu_clock *clock);
enum tg_gpu_clock_result tg_gpu_clock_measure(const struct tg_gpu_power_io *io,
    struct tg_gpu_clock *clock);
/* Quiescent GPCPLL transition: V-up + readback + ramp before F-up, and
 * F-down + counter verification before V-down. */
enum tg_gpu_clock_result tg_gpu_clock_set_rate(const struct tg_gpu_power_io *io,
    const struct tg_gpu_power *power, uint32_t target_khz, uint32_t required_uv,
    struct tg_gpu_clock *clock);
enum tg_gpu_clock_result tg_gpu_clock_stop(const struct tg_gpu_power_io *io,
    struct tg_gpu_clock *clock);

/* T210 CVB voltage for one OPP (uV), MAX77621-rounded; 0 = unavailable. */
uint32_t t210_gpu_clock_required_uv(uint32_t gpu_speedo, uint32_t target_khz);
/* GPCPLL COEFF word for one OPP; 0 = unknown frequency. */
uint32_t t210_gpu_clock_coeff(uint32_t target_khz);

/* ---- T214-compatible "hiopt" DVFS surface over the T210 OPP ladder ----
 * windows/dvfs.c is written against the T214 16-entry HighOPT table. The T210
 * ladder has 12 rows (76800..921600), so these wrappers keep dvfs.c unchanged:
 * frequencies/indices come from t210_opp, voltages from the T210 CVB rows plus
 * the EMC floor, and the thermal range stays a 6-step index. */
#define TG_GPU_HIOPT_OPP_COUNT  12u
#define TG_GPU_HIOPT_IDLE_KHZ   76800u
#define TG_GPU_HIOPT_SAFE_KHZ   460800u
#define TG_GPU_HIOPT_DOCK_KHZ   921600u
#define TG_GPU_HIOPT_MAX_KHZ    921600u
#define TG_GPU_HIOPT_OC_MAX_UV  900000u
#define TG_GPU_HIOPT_VMIN_UV    590000u
#define TG_GPU_THERMAL_RANGES   6u

uint32_t tg_gpu_clock_hiopt_freq_khz(uint32_t index);
uint32_t tg_gpu_clock_hiopt_index(uint32_t target_khz);
uint32_t tg_gpu_clock_thermal_range(int32_t temp_c, uint32_t prev_range);
uint32_t tg_gpu_clock_emc_floor_uv(uint32_t gpu_speedo, uint32_t emc_khz);
uint32_t tg_gpu_clock_required_uv(uint32_t gpu_speedo, uint32_t target_khz);
uint32_t tg_gpu_clock_hiopt_uv(uint32_t gpu_speedo, uint32_t target_khz,
                               uint32_t thermal_range, uint32_t emc_khz);


#endif



