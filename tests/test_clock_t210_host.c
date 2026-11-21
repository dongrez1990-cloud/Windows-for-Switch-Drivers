/* SPDX-License-Identifier: MIT
 * Host (PC) test for the T210 clock layer.
 *
 * Covers the 2026-11-21 P0-1 frequency-ladder work without a console reboot:
 *   - every OPP row is a legal boot point, off-ladder frequencies are refused;
 *   - the CVB voltage that the real chip produced for speedo 2031
 *     (868,750 / 1,000,000 / 1,050,000 / 1,106,250 uV at
 *     460.8 / 691.2 / 768 / 844.8 MHz) is reproduced by t210_opp;
 *   - 921.6 MHz is refused with TG_GPU_CLOCK_RAIL_ERROR because CVB asks for
 *     1,162,500 uV > max_mv 1113 mV, instead of quietly clocking the PLL at
 *     whatever rail the PMIC happens to hold.
 *
 * The register block, the PMIC and the clock counter are modelled after the
 * records the driver wrote on hardware (ClkNote3..7 of the 2026-11-21 runs).
 */
/* This dev box has clang + the Windows SDK headers but no MSVC vcruntime, so the
 * test is built with /nodefaultlib against Kernel32 only: output goes through
 * WriteFile and assert() is a local macro. */
typedef __SIZE_TYPE__ size_t;
#define NULL ((void *)0)
extern void *__stdcall GetStdHandle(int which);
extern int __stdcall WriteFile(void *handle, const void *buffer, unsigned long bytes,
                               unsigned long *written, void *overlapped);
extern void __stdcall ExitProcess(unsigned int code);
#define STD_OUTPUT_HANDLE (-11)

void *memset(void *dst, int value, size_t bytes)
{
    unsigned char *p = (unsigned char *)dst;
    while (bytes--) *p++ = (unsigned char)value;
    return dst;
}
void *memcpy(void *dst, const void *src, size_t bytes)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (bytes--) *d++ = *s++;
    return dst;
}

static void out_text(const char *text)
{
    unsigned long written = 0;
    unsigned long length = 0;
    while (text[length]) ++length;
    if (length) WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, length, &written, NULL);
}
static void out_uint(unsigned long value, unsigned base, int pad_to)
{
    char buffer[24];
    const char *digits = "0123456789ABCDEF";
    int length = 0;
    do {
        buffer[length++] = digits[value % base];
        value /= base;
    } while (value && length < 23);
    while (pad_to > length) { out_text("0"); --pad_to; }
    while (length) { char c[2]; c[0] = buffer[--length]; c[1] = 0; out_text(c); }
}
static void out_line(const char *text) { out_text(text); out_text("\r\n"); }

void host_fail(const char *expr, int line)
{
    out_text("ASSERT FAILED (line ");
    out_uint((unsigned long)line, 10, 0);
    out_text("): ");
    out_line(expr);
    ExitProcess(1);
}
#define assert(expr) \
    do { if (!(expr)) host_fail(#expr, __LINE__); } while (0)
#define CHECK(expr) assert(expr)

#include "../../../erista-gpu/src/mesa-switch-woa/tegra-x1-wddm/platform/t210/t210_clock.h"


/* Mirror hooks live in t210_power.c (kernel-only); the ladder logic does not
 * depend on them beyond "must not crash". */
void tg_gpu_clock_mark(const struct tg_gpu_power_io *io, uint32_t step)
{ (void)io; (void)step; }
void tg_gpu_clock_note(const struct tg_gpu_power_io *io, uint32_t slot, uint32_t value)
{ (void)io; (void)slot; (void)value; }

#include "../../../erista-gpu/src/mesa-switch-woa/tegra-x1-wddm/platform/t210/t210_clock.c"

/* ---- hardware model ---------------------------------------------------- */
#define MODEL_REGS 32
static struct { uint32_t address, value; } model[MODEL_REGS];
static uint32_t model_count;
static uint32_t target_khz;
static uint8_t pmic_vout = 0x80;          /* enabled, code 0 */
static uint32_t speedo_word = 2031u;
/* Fused ADC: slope 10.000 uV/code, intercept 600.000 uV (FUSE+0x204 layout). */
static uint32_t adc_fuse = (1u << 30) | (10u << 24) | (600u << 4);
static int pll_enable_writes;

static uint32_t *slot(uint32_t address, int create)
{
    uint32_t i;
    for (i = 0; i < model_count; ++i)
        if (model[i].address == address) return &model[i].value;
    if (!create || model_count == MODEL_REGS) return NULL;
    model[model_count].address = address;
    model[model_count].value = 0;
    return &model[model_count++].value;
}

static uint32_t m_read(void *context, uint32_t address)
{
    uint32_t *p;
    (void)context;
    if (address == 0x7000f930u) return speedo_word;
    if (address == 0x7000fb04u) return adc_fuse;
    /* The clock counter: measured_khz = count * 38400 / 800 = count * 48. */
    if (address == GPU + COUNT_VAL) return target_khz / 48u;
    p = slot(address, 0);
    return p ? *p : 0u;
}

static void m_write(void *context, uint32_t address, uint32_t value)
{
    uint32_t *p;
    (void)context;
    p = slot(address, 1);
    if (!p) return;
    *p = value;
    if (address == GPU + CFG && (value & ENABLE)) {
        ++pll_enable_writes;
        /* NA mode reports "calibrated" through DVFS1 bit31. */
        *slot(GPU + DVFS1, 1) |= 0x80000000u;
    }
}

static bool m_read_pmic(void *context, uint8_t address, uint8_t reg, uint8_t *value)
{
    (void)context; (void)address;
    if (reg == MAX77621_VOUT) { *value = pmic_vout; return true; }
    if (reg == MAX77621_VOUT_DVC) { *value = pmic_vout; return true; }
    return false;
}

static void m_write_pmic(void *context, uint8_t address, uint8_t reg, uint8_t value)
{
    (void)context; (void)address;
    if (reg == MAX77621_VOUT || reg == MAX77621_VOUT_DVC) pmic_vout = value;
}

static bool m_write_pmic_bool(void *context, uint8_t address, uint8_t reg, uint8_t value)
{ m_write_pmic(context, address, reg, value); return true; }

static void m_delay(void *context, uint32_t us) { (void)context; (void)us; }

static struct tg_gpu_power_io make_io(void)
{
    struct tg_gpu_power_io io;
    io.context = NULL;
    io.read32 = m_read;
    io.write32 = m_write;
    io.read_pmic = m_read_pmic;
    io.write_pmic = m_write_pmic_bool;
    io.delay_us = m_delay;
    io.release_pmic = NULL;
    return io;
}

static void model_reset(void)
{
    memset(model, 0, sizeof(model));
    model_count = 0;
    pmic_vout = 0x80;
    speedo_word = 2031u;
    adc_fuse = (1u << 30) | (10u << 24) | (600u << 4);
    pll_enable_writes = 0;
    /* Leave the GPU in its documented reset state: PLL off, VCO on bypass. */
    *slot(GPU + CFG, 1) = 0x00000002u;   /* IDDQ only */
    *slot(GPU + VCO, 1) = 0;
}

static void expect_boot_point(uint32_t khz, uint32_t expected_uv, uint32_t expected_cal)
{
    struct tg_gpu_power power;
    struct tg_gpu_clock clock;
    struct tg_gpu_power_io io = make_io();
    enum tg_gpu_clock_result result;

    model_reset();
    target_khz = khz;
    power = (struct tg_gpu_power){0};
    power.owned = true;
    power.ready = true;
    power.pmic_address = 0x1Cu;
    clock = (struct tg_gpu_clock){0};

    result = tg_gpu_clock_start(&io, &power, khz, &clock);
    if (result != TG_GPU_CLOCK_OK) {
        out_text("FAIL "); out_uint(khz, 10, 0); out_text(" kHz -> result "); out_uint((unsigned long)result, 10, 0); out_line("");
        assert(0);
    }
    out_text("  "); out_uint(khz, 10, 0);
    out_text(" kHz: rail "); out_uint(clock.rail_uv, 10, 0);
    out_text(" uV, measured "); out_uint(clock.measured_khz, 10, 0);
    out_text(" kHz, ext_cal "); out_uint((unsigned long)clock.external_calibration, 10, 0);
    out_line("");
    assert(clock.rail_uv == expected_uv);
    assert(clock.measured_khz == khz);
    assert(clock.target_khz == khz);
    assert(clock.external_calibration == (int32_t)expected_cal);
    assert(clock.running && clock.owned);
    /* The rail really was programmed, with the enable bit still set. */
    assert((pmic_vout & MAX77621_ENABLE) != 0);
    assert((pmic_vout & 0x7Fu) == max77621_code(expected_uv));
    assert(tg_gpu_clock_stop(&io, &clock) == TG_GPU_CLOCK_OK);
}

int run_tests(void)
{
    struct tg_gpu_power power;
    struct tg_gpu_clock clock;
    struct tg_gpu_power_io io;
    uint32_t i;

    /* 1) Every row of the OPP ladder is a legal boot point (P0 ladder). */
    for (i = 0; i < T210_GPU_OPP_COUNT; ++i)
        assert(t210_gpu_index(t210_gpu_pll[i].khz) == i);

    /* 2) The four points measured on the console on 2026-11-21, with the
     *    voltage and external calibration the real PMIC/PLL produced. */
    expect_boot_point(460800u, 868750u, 27u);
    expect_boot_point(691200u, 1000000u, 40u);
    expect_boot_point(768000u, 1050000u, 45u);
    expect_boot_point(844800u, 1106250u, 51u);

    /* 3) 921.6 MHz asks for 1,162,500 uV > max_mv: refuse instead of running
     *    the PLL on whatever rail is already programmed. */
    model_reset();
    target_khz = 921600u;
    power = (struct tg_gpu_power){0};
    power.owned = true;
    power.ready = true;
    power.pmic_address = 0x1Cu;
    clock = (struct tg_gpu_clock){0};
    io = make_io();
    assert(tg_gpu_clock_start(&io, &power, 921600u, &clock) == TG_GPU_CLOCK_RAIL_ERROR);
    assert(!clock.owned && !clock.running);
    assert(pll_enable_writes == 0);      /* the PLL was never enabled */
    assert((pmic_vout & 0x7Fu) == 0);    /* and the rail was left alone */
    out_line("921600 kHz refused (CVB 1,162,500 uV > max 1,113,000 uV)");

    /* 4) Off-ladder frequencies still fail loudly, with no register writes. */
    model_reset();
    clock = (struct tg_gpu_clock){0};
    io = make_io();
    assert(tg_gpu_clock_start(&io, &power, 1234567u, &clock) == TG_GPU_CLOCK_BAD_STATE);
    assert(!clock.owned && pll_enable_writes == 0);
    assert(tg_gpu_clock_start(&io, &power, 500000u, &clock) == TG_GPU_CLOCK_BAD_STATE);
    assert(!clock.owned && pll_enable_writes == 0);
    out_line("off-ladder frequencies refused");

    /* 5) Low OPP rows stay legal (the DVFS ladder uses them). */
    expect_boot_point(76800u, 800000u, 20u);
    expect_boot_point(307200u, 800000u, 20u);

    out_line("T210 clock ladder + CVB envelope guard tests passed.");
    return 0;
}

/* Linked with /nodefaultlib (no MSVC vcruntime on this dev box). */
void host_main(void) { int rc = run_tests(); ExitProcess((unsigned int)rc); }
