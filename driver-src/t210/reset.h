/* SPDX-License-Identifier: MIT
 * T210 (Erista) GPU reset ops ??draft contract for the tegra-x1-wddm
 * T210 platform layer.
 *
 * Verified local facts:
 *   - TEGRA210_CLK_GPU reset belongs to the CAR block (0x60006000).
 *   - The GPU node in socbase.dtsi is disabled by the bootloader and opened
 *     by the driver; this platform layer performs the actual reset.
 *
 * Reset register offsets (RST_DEVICES_x / RST_DEVICE_CLR_x) are NOT verified on
 * this tree. Port from:
 *   - Linux: drivers/clk/tegra/clk-tegra210.c (GPURST, RST_DEVICES_Y/W)
 *   - nouveau: nvkm/engine/device/tegra.c / nvkm/subdev/fb/gm20b.c reset path
 *   - The Lite reference: tegra-x1-wddm/platform/t214 reset implementation.
 */
#ifndef SWT210_RESET_H
#define SWT210_RESET_H
#include <stdint.h>
#include <stdbool.h>

struct swt210_reset_io {
    void *context;
    uint32_t (*read)(void *, uint32_t offset);
    void (*write)(void *, uint32_t offset, uint32_t value);
    void (*delay_us)(void *, uint32_t us);
};

/* TODO(clk-tegra210): hold the GPU reset via the CAR, then release it and
 * verify the block came out of reset (a read must not return all-ones). */
bool swt210_gpu_reset(const struct swt210_reset_io *io);

/* TODO(clk-tegra210): assert reset without touching other CAR clients. */
bool swt210_gpu_reset_hold(const struct swt210_reset_io *io);

/* TODO(clk-tegra210): release reset and confirm the block is responsive. */
bool swt210_gpu_reset_release(const struct swt210_reset_io *io);
#endif

