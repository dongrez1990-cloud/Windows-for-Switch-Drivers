/* SPDX-License-Identifier: MIT
 * T210 (Erista) MAX77621 VDD_GPU / VDD_CPU rail helper ??draft for the
 * tegra-x1-wddm T210 platform layer.
 *
 * Register facts verified against this machine's winx UEFI source
 * (Drivers/UsbPowerAuditDxe/UsbPowerAudit.c):
 *   - I2C5 0x1B = CPU rail, 0x1C = GPU rail
 *   - VOUT    (0x00): bit7 = enable, bits6:0 = voltage code
 *   - VOUT_DVC(0x01): dynamic-voltage code (write both on change)
 *   - VOUT = 606250 uV + (code & 0x7F) * 6250 uV
 *     => code 0xB7 = 950 mV, code 0xC3 = 1025 mV (R364/R365 evidence)
 *   - ODN CPU table: 1020 MHz needs 985 mV under DFLL; boot uses 1025 mV
 *     (open-loop PLLX margin), max 1227 mV.
 *
 * This is only the register math and vote bookkeeping. The caller owns the
 * I2C transfer, serialization and the rail's power-up sequence.
 * GPU OPP voltage table values (per process) belong to the platform layer and
 * still need to be sourced from L4T tegra210-dvfs.c (see ERISTA-DVFS-TABLES.md).
 */
#ifndef SWT210_PMIC_MAX77621_H
#define SWT210_PMIC_MAX77621_H
#include <stdint.h>
#include <stdbool.h>

#define MAX77621_GPU_ADDR   0x1Cu
#define MAX77621_CPU_ADDR   0x1Bu
#define MAX77621_VOUT       0x00u
#define MAX77621_VOUT_DVC   0x01u
#define MAX77621_VOUT_ENABLE 0x80u
#define MAX77621_MIN_UV     606250u
#define MAX77621_STEP_UV    6250u
#define MAX77621_ODN_MAX_UV 1227000u
#define MAX77621_CODE_MASK  0x7Fu

struct sw_max77621 {
    uint32_t boot_uv;    /* first observed (firmware) setpoint */
    uint32_t vote_uv;    /* highest outstanding client request, 0 = none */
    uint32_t applied_uv; /* last verified readback, 0 = unknown */
};

struct sw_max77621_io {
    void *context;
    bool (*read)(void *, uint8_t reg, uint8_t *value);
    bool (*write)(void *, uint8_t reg, uint8_t value);
    void (*settle)(void *); /* >=1 ms after a verified VOUT write */
};

static inline uint32_t sw_max77621_uv(uint8_t code)
{
    return MAX77621_MIN_UV + (uint32_t)(code & MAX77621_CODE_MASK) * MAX77621_STEP_UV;
}

/* Round UP to the next code so the rail never undershoots the request. */
static inline uint8_t sw_max77621_code(uint32_t uv)
{
    if (uv <= MAX77621_MIN_UV) return 0u;
    return (uint8_t)((uv - MAX77621_MIN_UV + MAX77621_STEP_UV - 1u) / MAX77621_STEP_UV);
}

/* Vote a minimum voltage on one rail (caller serializes accesses).
 * floor_uv keeps the firmware's own setpoint: other fixed-rate clients still
 * depend on it. A failed write may have changed the rail: keep the higher
 * vote and only report applied_uv from a successful readback. */
static inline bool sw_max77621_vote(struct sw_max77621 *s,
                                    const struct sw_max77621_io *io, bool set,
                                    uint32_t minimum_uv, uint32_t max_uv)
{
    uint8_t before = 0, target, after = 0;
    uint32_t required, old_vote = s->vote_uv;
    if (!s || !io) return false;
    if (set && minimum_uv &&
        (minimum_uv < MAX77621_MIN_UV || minimum_uv > max_uv)) return false;
    if (!io->read(io->context, MAX77621_VOUT, &before)) return false;
    s->applied_uv = sw_max77621_uv(before);
    if (!s->boot_uv) {
        if (s->applied_uv > max_uv) return false;
        s->boot_uv = s->applied_uv;
    }
    if (!set) return true;
    required = minimum_uv > s->boot_uv ? minimum_uv : s->boot_uv;
    if (required > max_uv) return false;
    target = (uint8_t)(MAX77621_VOUT_ENABLE | sw_max77621_code(required));
    /* Keep the higher vote on error; a failed down-write must not authorize
     * raising a clock. */
    s->vote_uv = minimum_uv > old_vote ? minimum_uv : old_vote;
    if (target != (before & (MAX77621_VOUT_ENABLE | MAX77621_CODE_MASK))) {
        if (!io->write(io->context, MAX77621_VOUT_DVC, target) ||
            !io->write(io->context, MAX77621_VOUT, target)) {
            s->applied_uv = io->read(io->context, MAX77621_VOUT, &after)
                                ? sw_max77621_uv(after) : 0;
            return false;
        }
    }
    if (!io->read(io->context, MAX77621_VOUT, &after)) { s->applied_uv = 0; return false; }
    s->applied_uv = sw_max77621_uv(after);
    if ((after & (MAX77621_VOUT_ENABLE | MAX77621_CODE_MASK)) != target) return false;
    io->settle(io->context);
    s->vote_uv = minimum_uv;
    return true;
}
#endif

