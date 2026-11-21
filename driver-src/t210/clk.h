/* SPDX-License-Identifier: MIT
 * T210 (Erista) GPU clock platform ops ??draft contract for the
 * tegra-x1-wddm T210 platform layer.
 *
 * Verified local facts:
 *   - CLOCK_BASE (CAR) = 0x60006000   (WoA\winx\Include\Device\T210.h:29)
 *   - HOST1X_BASE     = 0x50000000   (T210.h:20)
 *   - GPU apertures   = 0x57000000 / 0x58000000 (16 MiB), 0x538f0000 (4 KiB)
 *   - TEGRA210_CLK_GPU owns the reset: resets = <&tegra_car TEGRA210_CLK_GPU>
 *
 * Register offsets and the PLLGP/C1 program sequence are NOT yet verified on
 * this tree. Port them from:
 *   - nouveau: nvkm/subdev/clk/gm20b.c (gm20b_clk_prog, GM20B_CLK_*)
 *   - Linux:   drivers/clk/tegra/clk-tegra210.c (TEGRA210_CLK_GPU,
 *              PLLGP/C1 regs, CLK_OUT_ENB_Y GPU bit)
 *   - switchroot l4t-kernel-nvgpu: drivers/gpu/nvgpu/common/clock/...
 *   - the Lite reference: tegra-x1-wddm/platform/ (T214) once the source is
 *     available; only the offsets/values change for T210.
 * Do not guess offsets: every value below must be sourced before use.
 */
#ifndef SWT210_CLK_H
#define SWT210_CLK_H
#include <stdint.h>
#include <stdbool.h>

#define SWT210_CAR_BASE      0x60006000u
#define SWT210_HOST1X_BASE   0x50000000u
#define SWT210_GPU_WIN0      0x57000000u
#define SWT210_GPU_WIN1      0x58000000u
#define SWT210_GPU_MISC      0x538f0000u

struct swt210_clk_io {
    void *context;
    uint32_t (*read)(void *, uint32_t offset);
    void (*write)(void *, uint32_t offset, uint32_t value);
    void (*delay_us)(void *, uint32_t us);
};

struct swt210_gpu_clk {
    uint32_t gpcclk_khz;   /* current GPCCLK */
    uint32_t gpcpclk_khz;  /* GPCPLL VCO */
    bool busy;
};

/* TODO(nouveau): port the C1/NA PLL program sequence before use. */
void swt210_gpu_clock_init(const struct swt210_clk_io *io);

/* TODO(nouveau): enable PLLGP + GPCCLK mux and report the achieved rate. */
bool swt210_gpu_clock_enable(const struct swt210_clk_io *io,
                             struct swt210_gpu_clk *clk, uint32_t khz);

/* TODO(nouveau): read back the live GPCCLK counter for a real sample. */
bool swt210_gpu_clock_sample(const struct swt210_clk_io *io,
                             struct swt210_gpu_clk *clk);
#endif

