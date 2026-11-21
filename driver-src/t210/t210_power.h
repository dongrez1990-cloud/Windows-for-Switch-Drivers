/* SPDX-License-Identifier: GPL-2.0-only
 * T210 (Erista) GPU power sequencing ??port of tegra-x1-wddm's
 * platform/t210_power.[ch] for the GM20B on T210.
 *
 * The caller serializes shared CAR/PMC/MC and I2C5 accesses. This interface
 * acquires an idle, reset, clamped GPU only. It does not change CPU/DDR
 * voltage, PLLP itself, GPCPLL, memory mappings or GPU firmware.
 *
 * T210 deltas vs T214 (verified sources):
 *  - HIDREV 0x70000804 reports 0x211x on Erista; 0x212x is T210B01.
 *  - VDD_GPU is MAX77621 at I2C5 0x1C (VOUT 0x00 / VOUT_DVC 0x01, 6.25 mV
 *    steps from 606.25 mV, bit7 = enable), not MAX77812 M1.
 *  - CAR/PMC/MC register offsets are shared and re-verified by the board
 *    check below; see the numbers in the code and ERISTA-DVFS-TABLES.md.
 */
#ifndef TEGRA_GPU_T210_POWER_H
#define TEGRA_GPU_T210_POWER_H

#include <stdbool.h>
#include <stdint.h>

struct tg_gpu_power_io {
    void *context;
    uint32_t (*read32)(void *context, uint32_t address);
    void (*write32)(void *context, uint32_t address, uint32_t value);
    bool (*read_pmic)(void *context, uint8_t address, uint8_t reg, uint8_t *value);
    bool (*write_pmic)(void *context, uint8_t address, uint8_t reg, uint8_t value);
    void (*delay_us)(void *context, uint32_t delay);
    bool (*release_pmic)(void *context, uint8_t address,
                         uint8_t original_enable, uint8_t original_voltage);
};

enum tg_gpu_power_result {
    TG_GPU_POWER_OK,
    TG_GPU_POWER_UNSUPPORTED_BOARD,
    TG_GPU_POWER_BUSY,
    TG_GPU_POWER_PMIC_ERROR,
    TG_GPU_POWER_CLOCK_ERROR,
    TG_GPU_POWER_RESET_ERROR,
    TG_GPU_POWER_MC_TIMEOUT,
    TG_GPU_POWER_BAD_ARGUMENT
};

enum tg_gpu_power_stage {
    TG_GPU_STAGE_SNAPSHOT,
    TG_GPU_STAGE_RAIL,
    TG_GPU_STAGE_CLOCKS,
    TG_GPU_STAGE_RESET,
    TG_GPU_STAGE_MC,
    TG_GPU_STAGE_READY
};

struct tg_gpu_power {
    bool owned;
    bool ready;
    uint8_t pmic_address;
    uint8_t original_enable;
    uint8_t original_voltage;
    uint32_t original_clocks;
    uint32_t original_pllp_out5;
    uint32_t original_mc_flush;
    uint32_t original_gpu_rg;
    enum tg_gpu_power_stage stage;
};

enum tg_gpu_power_result tg_gpu_power_up(const struct tg_gpu_power_io *io,
                                        uint8_t pmic_address,
                                        struct tg_gpu_power *state);
enum tg_gpu_power_result tg_gpu_power_down(const struct tg_gpu_power_io *io,
                                          struct tg_gpu_power *state);
enum tg_gpu_power_result tg_gpu_power_drain(const struct tg_gpu_power_io *io,
                                           const struct tg_gpu_power *state);

/* Persist a bring-up step marker from another platform unit (the clock stage),
 * so a bus stall inside it still leaves the failing sub-step in the mirror. */
void tg_gpu_clock_mark(const struct tg_gpu_power_io *io, uint32_t step);
void tg_gpu_clock_note(const struct tg_gpu_power_io *io, uint32_t slot, uint32_t value);

#endif



