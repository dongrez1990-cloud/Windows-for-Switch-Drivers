/* SPDX-License-Identifier: MIT
 * T210 (Erista) GM20B OPP ladder and CVB voltage model.
 *
 * Sources (extracted into src/l4t-kernel/extracted/):
 *  - drivers/soc/tegra/tegra210-dvfs.c: FIXED_FREQ_CVB_TABLE coefficients and
 *    gpu_cvb_dvfs_table speedo limits (retail: speedo 4, pll_min 918 mV,
 *    max 1113 mV at 844800 kHz; DTS gpu-dvfs-rework allows 921600).
 *  - drivers/soc/tegra/cvb.c: tegra_get_cvb_voltage():
 *      mv = round(round(c2*speedo/s_scale + c1) * speedo/s_scale + c0)
 *    with s_scale=100 and voltage_scale=1000 (mV -> uV*1000).
 *  - tegra-x1-wddm platform/t210_clock.c: N/PL pairs for the shared GM20B
 *    GPCPLL (VCO = 38.4 MHz * N must stay 1300..2600 MHz, SDM=-4096).
 */
#ifndef TEGRA_GPU_T210_OPP_H
#define TEGRA_GPU_T210_OPP_H
#include <stdint.h>

#define T210_GPU_OPP_COUNT 12u

/* FIXED_FREQ_CVB_TABLE rows (76800..921600): {freq, {}, {c0, c1, c2}}. */
struct t210_gpu_cvb {
    uint32_t khz;
    int32_t c0, c1, c2;
};

static const struct t210_gpu_cvb t210_gpu_cvb[T210_GPU_OPP_COUNT] = {
    {  76800u, 1786666, -85625, 1632 },
    { 153600u, 1846729, -87525, 1632 },
    { 230400u, 1910480, -89425, 1632 },
    { 307200u, 1977920, -91325, 1632 },
    { 384000u, 2049049, -93215, 1632 },
    { 460800u, 2122872, -95095, 1632 },
    { 537600u, 2201331, -96985, 1632 },
    { 614400u, 2283479, -98885, 1632 },
    { 691200u, 2369315, -100785, 1632 },
    { 768000u, 2458841, -102685, 1632 },
    { 844800u, 2550821, -104555, 1632 },
    { 921600u, 2647676, -106455, 1632 },
};

/* GPCPLL N/PL pairs for the same frequencies (VCO = 38.4 MHz * N). */
struct t210_gpu_pll {
    uint32_t khz;
    uint8_t n, pl;
};

static const struct t210_gpu_pll t210_gpu_pll[T210_GPU_OPP_COUNT] = {
    {  76800u, 36u, 9u }, { 153600u, 40u, 5u }, { 230400u, 36u, 3u },
    { 307200u, 48u, 3u }, { 384000u, 40u, 2u }, { 460800u, 48u, 2u },
    { 537600u, 56u, 2u }, { 614400u, 64u, 2u }, { 691200u, 36u, 1u },
    { 768000u, 40u, 1u }, { 844800u, 44u, 1u }, { 921600u, 48u, 1u },
};

uint32_t t210_gpu_freq_khz(uint32_t index);
uint32_t t210_gpu_index(uint32_t khz);
/* CVB voltage (uV) for one OPP row, MAX77621-rounded; 0 = unavailable. */
uint32_t t210_gpu_cvb_uv(uint32_t speedo, uint32_t khz);

#endif



