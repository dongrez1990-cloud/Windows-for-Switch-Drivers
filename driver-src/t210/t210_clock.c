/* SPDX-License-Identifier: MIT
 * T210 (Erista) GM20B clock layer ??adapted from tegra-x1-wddm
 * platform/t214_clock.c. GPCPLL register sequence follows Switchroot nvgpu
 * clk_gm20b.c (MIT); CVB coefficients come from the L4T T210 tables
 * (tegra210-dvfs.c) via t210_opp.h.
 */
#include "t210_clock.h"
#include "t210_opp.h"
#include "t210_emc_gpu_floor.h"

#define GPU 0x57000000u
#define CFG 0x137000u
#define COEFF 0x137004u
#define CFG2 0x13700cu
#define CFG3 0x137018u
#define DVFS0 0x137010u
#define DVFS1 0x137014u
#define DVFS2 0x132820u
#define VCO 0x137100u
#define OUT 0x137250u
#define BYPASS 0x137340u
#define SLOW 0x020160u
#define THERM 0x020798u
#define SVOP 0x021944u
#define SVOP_OVERRIDE 0x021948u
#define COUNT_CFG 0x134124u
#define COUNT_VAL 0x134128u
#define ENABLE 1u
#define IDDQ 2u
#define SYNC 4u

/* MAX77621 GPU rail on I2C5 0x1C. */
#define MAX77621_VOUT 0x00u
#define MAX77621_VOUT_DVC 0x01u
#define MAX77621_ENABLE 0x80u
#define MAX77621_MIN_UV 606250u
#define MAX77621_STEP_UV 6250u

static const uint32_t clock_regs[TG_GPU_CLOCK_SAVED_REGS] = {
    CFG, COEFF, CFG2, CFG3, DVFS0, DVFS1, DVFS2, VCO, OUT, BYPASS,
    SVOP, SVOP_OVERRIDE, SLOW
};
static const uint32_t clock_masks[TG_GPU_CLOCK_SAVED_REGS] = {
    0x17, 0x3fffff, 0xff0000ff, 0x00ff01ff, 0x00007f7f,
    0x7fff0000, 0x0001007f, 1, 0x80003f3f, 1, 3, 1, 0x003f0000
};

static uint32_t clk_read(const struct tg_gpu_power_io *io, uint32_t reg)
{ return io->read32(io->context, GPU + reg); }

/* Erista returns a "block not accessible" sentinel for part of the GPCPLL page
 * (0x137000/0x137004/0x13700c read 0xBADF3000 while 0x137100/0x137250 answer).
 * A read-modify-write on those slots folds the sentinel bits into the PLL
 * configuration, so the following SYNC/enable writes stall the APB. Track a
 * shadow value instead and never propagate a sentinel back into the hardware. */
static bool clk_bad(uint32_t value)
{
    return value == 0xFFFFFFFFu || (value & 0xFFFF0000u) == 0xBAD00000u ||
           (value & 0xFFFF0000u) == 0xBADF0000u;
}

static uint32_t clk_shadow(uint32_t reg, uint32_t value, bool store)
{
    static struct { uint32_t reg; uint32_t value; } shadow[16];
    uint32_t index;
    for (index = 0; index < 16u; ++index) {
        if (shadow[index].reg == reg) {
            if (store) shadow[index].value = value;
            return shadow[index].value;
        }
        if (shadow[index].reg == 0u) {
            shadow[index].reg = reg ? reg : 1u;
            shadow[index].value = value;
            return value;
        }
    }
    return value;
}

static void clk_write(const struct tg_gpu_power_io *io, uint32_t reg, uint32_t val)
{
    io->write32(io->context, GPU + reg, val);
    (void)clk_shadow(reg, val, true);
    (void)clk_read(io, reg);
}

static void clk_update(const struct tg_gpu_power_io *io, uint32_t reg,
                       uint32_t mask, uint32_t val)
{
    uint32_t old = clk_read(io, reg);
    if (clk_bad(old)) old = clk_shadow(reg, 0u, false);
    clk_write(io, reg, (old & ~mask) | (val & mask));
}

static void clk_select(const struct tg_gpu_power_io *io, uint32_t vco)
{
    uint32_t saved = clk_read(io, THERM);
    clk_write(io, THERM, 0);
    clk_update(io, VCO, 1, vco);
    clk_write(io, THERM, saved);
}

static int32_t div_nearest(int32_t n, int32_t d)
{ return n < 0 ? -((-n + d / 2) / d) : (n + d / 2) / d; }

static uint8_t max77621_code(uint32_t uv)
{
    if (uv <= MAX77621_MIN_UV) return 0u;
    uv = (uv - MAX77621_MIN_UV + MAX77621_STEP_UV - 1u) / MAX77621_STEP_UV;
    return (uint8_t)(uv > 0x7Fu ? 0x7Fu : uv);
}

uint32_t t210_gpu_clock_required_uv(uint32_t gpu_speedo, uint32_t target_khz)
{
    return t210_gpu_cvb_uv(gpu_speedo, target_khz);
}

uint32_t t210_gpu_clock_coeff(uint32_t target_khz)
{
    uint32_t i;
    for (i = 0; i < T210_GPU_OPP_COUNT; ++i)
        if (t210_gpu_pll[i].khz == target_khz)
            return 1u | ((uint32_t)t210_gpu_pll[i].n << 8) |
                   ((uint32_t)t210_gpu_pll[i].pl << 16);
    return 0u;
}

static bool write_rail(const struct tg_gpu_power_io *io, uint8_t address,
                       uint32_t target_uv, uint32_t settle_us)
{
    uint8_t readback = 0, expected;
    expected = (uint8_t)(MAX77621_ENABLE | max77621_code(target_uv));
    if (!io->write_pmic(io->context, address, MAX77621_VOUT_DVC, expected) ||
        !io->write_pmic(io->context, address, MAX77621_VOUT, expected) ||
        !io->read_pmic(io->context, address, MAX77621_VOUT, &readback) ||
        readback != expected)
        return false;
    io->delay_us(io->context, settle_us);
    return true;
}

enum tg_gpu_clock_result tg_gpu_clock_start(const struct tg_gpu_power_io *io,
    const struct tg_gpu_power *power, uint32_t target, struct tg_gpu_clock *c)
{
    uint32_t i, fuse, coeff, value, required_uv, mv;
    uint8_t vout = 0;
    if (!io || !power || !c || !io->read32 || !io->write32 ||
        !io->read_pmic || !io->write_pmic || !io->delay_us || !power->ready ||
        c->owned ||
        /* P0 frequency ladder: every row of the T210 OPP table (76800..921600)
         * is a legal boot point; an off-ladder frequency still fails loudly. */
        t210_gpu_index(target) == T210_GPU_OPP_COUNT)
        return TG_GPU_CLOCK_BAD_STATE;
    c->stage = 0;
    tg_gpu_clock_mark(io, 70);
    /* Freshly reset GPU only. Existing clock owners are never displaced. */
    if ((clk_read(io, CFG) & ENABLE) || (clk_read(io, VCO) & 1))
        return TG_GPU_CLOCK_BAD_STATE;
    tg_gpu_clock_mark(io, 71);
    /* Verify that the PLL core page actually answers before issuing enable/sync:
     * measured on Erista, once CFG had been folded with the 0xBADF3000 sentinel,
     * the SYNC (or VCO enable) write stalled the APB and blue-screened the boot.
     * Abort with a status instead, and record what the page looks like. */
    {
        uint32_t cfg0 = clk_read(io, CFG), coeff0 = clk_read(io, COEFF);
        tg_gpu_clock_note(io, 0, cfg0);
        tg_gpu_clock_note(io, 1, coeff0);
        tg_gpu_clock_note(io, 2, clk_read(io, 0x0200u));
        if (clk_bad(cfg0) || clk_bad(coeff0)) {
            clk_write(io, CFG, 0u);
            tg_gpu_clock_note(io, 3, clk_read(io, CFG));
            clk_write(io, COEFF, 0u);
            tg_gpu_clock_note(io, 4, clk_read(io, COEFF));
            tg_gpu_clock_note(io, 5, clk_read(io, CFG2));
            tg_gpu_clock_note(io, 6, clk_read(io, CFG3));
            tg_gpu_clock_note(io, 7, clk_read(io, 0x0137140u));
            tg_gpu_clock_mark(io, 83);
            return TG_GPU_CLOCK_READBACK_ERROR;
        }
    }
    /* GPU speedo fuse. The raw word at FUSE+0x130 is not a speedo value and the
     * bitfield position is still unverified on Erista, so accept it only when
     * it already looks like one (or when a plausible field can be extracted).
     * With no usable speedo the CVB table cannot be evaluated; keep the rail
     * that tg_gpu_power_up already set to the 800 mV bring-up value instead of
     * failing the start. */
    c->gpu_speedo = io->read32(io->context, 0x7000f930u);
    if (c->gpu_speedo == 0xFFFFFFFFu || c->gpu_speedo > 5000u) {
        uint32_t field = (c->gpu_speedo >> 8) & 0x3ffu;
        c->gpu_speedo = (field && field <= 5000u) ? field : 0u;
    }
    required_uv = c->gpu_speedo ? t210_gpu_clock_required_uv(c->gpu_speedo, target) : 0u;
    if (!io->read_pmic(io->context, power->pmic_address, MAX77621_VOUT, &vout) ||
        !(vout & MAX77621_ENABLE))
        return TG_GPU_CLOCK_RAIL_ERROR;
    if (c->gpu_speedo && (!required_uv || required_uv > 1113000u)) {
        /* The speedo is usable, so a 0 here is not "unknown": it means the CVB
         * row asked for more than max_mv (1113 mV) - e.g. 921600 kHz needs
         * 1,162,500 uV. Running that point at whatever the PMIC currently holds
         * would clock the GPU above its voltage envelope, so refuse it instead
         * of falling back to the rail in flight. (Old behaviour: silently ran
         * the PLL at the 800 mV bring-up rail.) */
        return TG_GPU_CLOCK_RAIL_ERROR;
    }
    if (required_uv && required_uv <= 1113000u) {
        /* PLL is still disabled; program voltage before frequency. */
        if (!write_rail(io, power->pmic_address, required_uv, 5000u))
            return TG_GPU_CLOCK_RAIL_ERROR;
        c->rail_uv = required_uv;
    } else {
        c->rail_uv = 606250u + (uint32_t)(vout & 0x7Fu) * 6250u;
        required_uv = c->rail_uv;
    }
    c->target_khz = target;
    /* Frequency-ladder forensics: the requested point, the CVB voltage the
     * speedo produced and the raw speedo word, so a higher step can be judged
     * from gpu-run.txt alone (flushed by the next mark). */
    tg_gpu_clock_note(io, 3, target);
    tg_gpu_clock_note(io, 4, required_uv);
    tg_gpu_clock_note(io, 6, c->gpu_speedo);
    mv = required_uv / 1000u;
    for (i = 0; i < TG_GPU_CLOCK_SAVED_REGS; ++i)
        c->saved[i] = clk_read(io, clock_regs[i]);
    c->owned = true;
    c->running = false;
    c->stage = 1;
    tg_gpu_clock_mark(io, 72);
    clk_select(io, 0);
    tg_gpu_clock_mark(io, 73);
    clk_update(io, OUT, 0x80003f3f, 0x80000000);
    tg_gpu_clock_mark(io, 74);
    clk_update(io, BYPASS, 1, 0);
    if (!(clk_read(io, SVOP) & 3)) {
        clk_update(io, SVOP, 3, 2);
        clk_update(io, SVOP_OVERRIDE, 1, 1);
    }
    tg_gpu_clock_mark(io, 75);
    clk_update(io, SLOW, 0x003f0000, 0);
    if (clk_read(io, SLOW) & 0x003f0000)
        return TG_GPU_CLOCK_READBACK_ERROR;
    tg_gpu_clock_mark(io, 76);

    /* NA mode uses C1 VCO_CTRL/DFS_CTRL. */
    clk_update(io, DVFS1, 0x2fff0000, 0x2c000000);
    clk_update(io, CFG3, 0x1ff, 0x0f);
    fuse = io->read32(io->context, 0x7000fb04u); /* FUSE + 0x100 + 0x204 */
    c->adc_slope_uv = (int32_t)(((fuse >> 24) & 0x3f) * 1000 + ((fuse >> 14) & 0x3ff));
    c->adc_intercept_uv = (int32_t)(((fuse >> 4) & 0x3ff) * 1000 + (fuse & 0xf) * 100);
    if (!(fuse >> 30) || !c->adc_slope_uv || !c->adc_intercept_uv) {
        c->adc_slope_uv = 0;
        c->adc_intercept_uv = 0;
    } else {
        clk_update(io, DVFS1, 1u << 30, 1u << 30);
    }
    clk_update(io, CFG, IDDQ, 0);
    io->delay_us(io->context, 5);
    clk_update(io, CFG2, 0xff000000, 0x04000000);
    clk_update(io, CFG3, 0x00ff0000, 0x00050000);
    c->stage = 2;
    tg_gpu_clock_mark(io, 77);
    if (!c->adc_slope_uv) {
        clk_update(io, DVFS1, 1u << 30, 1u << 30);
        clk_update(io, CFG, ENABLE, ENABLE);
        for (i = 0; i < 5; ++i) {
            if (clk_read(io, DVFS1) & (1u << 31)) break;
            io->delay_us(io->context, 1);
        }
        value = (clk_read(io, CFG3) >> 24) & 0x7f;
        clk_update(io, CFG, ENABLE, 0);
        if (i == 5) return TG_GPU_CLOCK_CALIBRATION_ERROR;
        c->adc_slope_uv = 10000;
        c->adc_intercept_uv = (int32_t)required_uv - (int32_t)value * 10000;
    }
    c->external_calibration =
        div_nearest((int32_t)required_uv - c->adc_intercept_uv, c->adc_slope_uv);
    tg_gpu_clock_note(io, 5, (uint32_t)c->external_calibration);
    if (c->external_calibration <= -64 || c->external_calibration >= 64)
        return TG_GPU_CLOCK_CALIBRATION_ERROR;

    c->stage = 3;
    tg_gpu_clock_mark(io, 78);
    clk_update(io, CFG, SYNC, 0);
    clk_update(io, CFG, ENABLE, 0);
    clk_update(io, DVFS2, 1u << 16, 1u << 16);
    value = (uint32_t)div_nearest(div_nearest(-172550 * (int32_t)mv, 1000) + 195374, 1000);
    if (value > 127) return TG_GPU_CLOCK_CALIBRATION_ERROR;
    clk_update(io, DVFS0, 0x7f7f, value);
    io->delay_us(io->context, 1);
    clk_update(io, DVFS2, 1u << 16, 0);
    clk_update(io, DVFS2, 0x7f, (uint32_t)c->external_calibration);
    io->delay_us(io->context, 1);
    clk_update(io, DVFS1, 0x0fff0000, 0x0e010000);
    /* SDM=-4096 (top byte f0) makes effective N equal integer N. */
    clk_update(io, CFG2, 0xff, 0xf0);
    coeff = t210_gpu_clock_coeff(target);
    if (!coeff) return TG_GPU_CLOCK_BAD_STATE;
    tg_gpu_clock_mark(io, 79);
    clk_update(io, COEFF, 0x3fffff, coeff);
    tg_gpu_clock_mark(io, 80);
    clk_update(io, CFG, ENABLE, ENABLE);
    io->delay_us(io->context, 40); /* NA mode has no usable LOCK bit. */
    tg_gpu_clock_mark(io, 81);
    clk_update(io, CFG, SYNC, SYNC);
    clk_select(io, 1);
    tg_gpu_clock_mark(io, 82);
    if ((clk_read(io, COEFF) & 0x3fffff) != coeff ||
        (clk_read(io, CFG) & (ENABLE | SYNC | IDDQ)) != (ENABLE | SYNC) ||
        !(clk_read(io, VCO) & 1) || (clk_read(io, CFG2) & 0xff) != 0xf0)
        return TG_GPU_CLOCK_READBACK_ERROR;
    c->stage = 4;
    c->running = true;
    return tg_gpu_clock_measure(io, c);
}

enum tg_gpu_clock_result tg_gpu_clock_measure(const struct tg_gpu_power_io *io,
    struct tg_gpu_clock *c)
{
    uint32_t slow, counter_cfg, first, second;
    if (!c->owned || !c->running) return TG_GPU_CLOCK_BAD_STATE;
    slow = clk_read(io, SLOW);
    counter_cfg = clk_read(io, COUNT_CFG);
    clk_update(io, SLOW, 0x3f0000, 0);
    clk_write(io, COUNT_CFG, 0x01000000);
    clk_write(io, COUNT_CFG, 0x00110000 | 800);
    io->delay_us(io->context, 200);
    first = clk_read(io, COUNT_VAL) & 0xfffff;
    io->delay_us(io->context, 100);
    second = clk_read(io, COUNT_VAL) & 0xfffff;
    clk_write(io, COUNT_CFG, counter_cfg);
    clk_write(io, SLOW, slow);
    c->counter = second;
    c->measured_khz = (uint32_t)((uint64_t)second * TG_GPU_REFERENCE_KHZ / 800);
    tg_gpu_clock_note(io, 7, c->measured_khz);
    tg_gpu_clock_mark(io, 84); /* flush the ladder notes before the range check */
    if (!first || first != second || c->measured_khz < c->target_khz * 995u / 1000u ||
        c->measured_khz > c->target_khz * 1005u / 1000u)
        return TG_GPU_CLOCK_COUNTER_ERROR;
    c->stage = 5;
    return TG_GPU_CLOCK_OK;
}

static bool program_pll_under_bypass(const struct tg_gpu_power_io *io,
    uint32_t coeff, uint32_t dfs_coeff, int32_t ext_cal)
{
    clk_select(io, 0);
    clk_update(io, CFG, SYNC, 0);
    clk_update(io, CFG, ENABLE, 0);
    clk_update(io, DVFS2, 1u << 16, 1u << 16);
    clk_update(io, DVFS0, 0x7f7f, dfs_coeff);
    io->delay_us(io->context, 1);
    clk_update(io, DVFS2, 1u << 16, 0);
    clk_update(io, DVFS2, 0x7f, (uint32_t)ext_cal);
    io->delay_us(io->context, 1);
    clk_update(io, DVFS1, 0x0fff0000, 0x0e010000);
    clk_update(io, CFG2, 0xff, 0xf0);
    clk_update(io, COEFF, 0x3fffff, coeff);
    clk_update(io, CFG, ENABLE, ENABLE);
    io->delay_us(io->context, 40);
    clk_update(io, CFG, SYNC, SYNC);
    clk_select(io, 1);
    return (clk_read(io, COEFF) & 0x3fffff) == coeff &&
           (clk_read(io, CFG) & (ENABLE | SYNC | IDDQ)) == (ENABLE | SYNC) &&
           (clk_read(io, VCO) & 1) != 0 &&
           (clk_read(io, CFG2) & 0xff) == 0xf0;
}

enum tg_gpu_clock_result tg_gpu_clock_set_rate(const struct tg_gpu_power_io *io,
    const struct tg_gpu_power *power, uint32_t target, uint32_t required_uv,
    struct tg_gpu_clock *c)
{
    uint32_t coeff, dfs_coeff, mv, old_khz, old_uv, old_coeff, old_dvfs0;
    int32_t ext_cal, old_cal;
    enum tg_gpu_clock_result res;
    if (!io || !power || !c || !io->read32 || !io->write32 || !io->read_pmic ||
        !io->write_pmic || !io->delay_us || !power->owned || !power->ready ||
        !c->owned || !c->running || !c->adc_slope_uv)
        return TG_GPU_CLOCK_BAD_STATE;
    coeff = t210_gpu_clock_coeff(target);
    if (!coeff || !required_uv || required_uv > 1113000u) return TG_GPU_CLOCK_RAIL_ERROR;
    ext_cal = div_nearest((int32_t)required_uv - c->adc_intercept_uv, c->adc_slope_uv);
    mv = required_uv / 1000u;
    dfs_coeff = (uint32_t)div_nearest(div_nearest(-172550 * (int32_t)mv, 1000) + 195374, 1000);
    if (ext_cal <= -64 || ext_cal >= 64 || dfs_coeff > 127u)
        return TG_GPU_CLOCK_CALIBRATION_ERROR;
    if (target == c->target_khz && required_uv == c->rail_uv)
        return TG_GPU_CLOCK_OK;

    old_khz = c->target_khz;
    old_uv = c->rail_uv;
    old_cal = c->external_calibration;
    old_coeff = clk_read(io, COEFF) & 0x3fffff;
    old_dvfs0 = clk_read(io, DVFS0) & 0x7f7f;

    /* Voltage UP first (with PMIC readback + ramp delay) before frequency UP. */
    if (required_uv > old_uv) {
        if (!write_rail(io, power->pmic_address, required_uv, 1000u)) {
            c->running = false;
            return TG_GPU_CLOCK_RAIL_ERROR;
        }
        c->rail_uv = required_uv;
    }
    if (!program_pll_under_bypass(io, coeff, dfs_coeff, ext_cal)) {
        res = TG_GPU_CLOCK_READBACK_ERROR;
    } else {
        c->target_khz = target;
        c->external_calibration = ext_cal;
        res = tg_gpu_clock_measure(io, c);
    }
    if (res != TG_GPU_CLOCK_OK) {
        /* Roll back PLL to the prior verified clock without lowering voltage. */
        c->target_khz = old_khz;
        c->external_calibration = old_cal;
        if (!program_pll_under_bypass(io, old_coeff, old_dvfs0, old_cal) ||
            tg_gpu_clock_measure(io, c) != TG_GPU_CLOCK_OK)
            c->running = false;
        return res;
    }
    /* Voltage DOWN only AFTER register readback and live counter confirm F-down. */
    if (required_uv < old_uv) {
        if (!write_rail(io, power->pmic_address, required_uv, 200u)) {
            c->running = false;
            return TG_GPU_CLOCK_RAIL_ERROR;
        }
        c->rail_uv = required_uv;
    }
    return TG_GPU_CLOCK_OK;
}

enum tg_gpu_clock_result tg_gpu_clock_stop(const struct tg_gpu_power_io *io,
    struct tg_gpu_clock *c)
{
    uint32_t i;
    bool ok = true;
    if (!c->owned) return TG_GPU_CLOCK_OK;
    clk_select(io, 0);
    clk_update(io, CFG, SYNC, 0);
    clk_update(io, CFG, ENABLE, 0);
    c->running = false;
    /* Restore CFG last, so the original IDDQ state follows other registers. */
    for (i = TG_GPU_CLOCK_SAVED_REGS; i-- > 0; ) {
        clk_update(io, clock_regs[i], clock_masks[i], c->saved[i]);
        if ((clk_read(io, clock_regs[i]) & clock_masks[i]) !=
            (c->saved[i] & clock_masks[i])) ok = false;
    }
    if (!ok) return TG_GPU_CLOCK_READBACK_ERROR;
    c->owned = false;
    return TG_GPU_CLOCK_OK;
}


/* ---- T214-compatible hiopt surface over the T210 OPP ladder ---- */
uint32_t tg_gpu_clock_hiopt_freq_khz(uint32_t index)
{
    return index < TG_GPU_HIOPT_OPP_COUNT ? t210_gpu_freq_khz(index) : 0u;
}

uint32_t tg_gpu_clock_hiopt_index(uint32_t target_khz)
{
    uint32_t i;
    for (i = 0; i < TG_GPU_HIOPT_OPP_COUNT; ++i)
        if (t210_gpu_freq_khz(i) == target_khz)
            return i;
    return UINT32_MAX;
}

uint32_t tg_gpu_clock_thermal_range(int32_t temp_c, uint32_t prev_range)
{
    static const int32_t trips[TG_GPU_THERMAL_RANGES - 1u] = { 20, 30, 50, 70, 90 };
    uint32_t r = 0;
    while (r < TG_GPU_THERMAL_RANGES - 1u && temp_c > trips[r])
        ++r;
    if (prev_range < TG_GPU_THERMAL_RANGES && r < prev_range) {
        /* 1 C downward hysteresis across thermal trip boundaries. */
        while (r < prev_range && temp_c > trips[r] - 1)
            ++r;
    }
    return r;
}

uint32_t tg_gpu_clock_emc_floor_uv(uint32_t gpu_speedo, uint32_t emc_khz)
{
    return tg_gpu_emc_floor_policy_uv(gpu_speedo, emc_khz);
}

uint32_t tg_gpu_clock_required_uv(uint32_t gpu_speedo, uint32_t target_khz)
{
    return t210_gpu_clock_required_uv(gpu_speedo, target_khz);
}

uint32_t tg_gpu_clock_hiopt_uv(uint32_t gpu_speedo, uint32_t target_khz,
                               uint32_t thermal_range, uint32_t emc_khz)
{
    uint32_t uv, emc_uv;
    (void)thermal_range; /* the T210 FIXED_FREQ CVB rows carry no temperature terms */
    uv = t210_gpu_clock_required_uv(gpu_speedo, target_khz);
    if (!uv)
        return UINT32_MAX;
    if (uv < TG_GPU_HIOPT_VMIN_UV)
        uv = TG_GPU_HIOPT_VMIN_UV;
    emc_uv = tg_gpu_clock_emc_floor_uv(gpu_speedo, emc_khz);
    if (emc_uv != UINT32_MAX && uv < emc_uv)
        uv = emc_uv;
    return uv <= TG_GPU_HIOPT_OC_MAX_UV ? uv : UINT32_MAX;
}