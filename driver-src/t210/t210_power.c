/* SPDX-License-Identifier: GPL-2.0-only
 * T210 (Erista) GPU power sequencing ??adapted from tegra-x1-wddm
 * platform/t214_power.c (Copyright 2026 ruje0504-2; NVIDIA/Switchroot
 * ordering references documented there). The T210 differences are the
 * HIDREV check and the MAX77621 0x1C rail.
 */
#include "t210_power.h"
#include <ntddk.h>


/* Bring-up bisection: a DWORD registry value "PowerSkipMask" under
 * services\tegragpurender\Parameters lets one power sub-step be skipped so a
 * hanging register write can be isolated without breaking cleanup:
 *   bit0 rail enable, bit1 CAR OUT5+clock ungate, bit2 reset release,
 *   bit3 MC flush drain, bit4 PMC clamp clear, bit5 = private memory
 *   allocation (hardware.c), bit6 PMC power gate, bit7 = frontend start
 *   (hardware.c). GPU rail-gate clamps (PMC+0x2D4) are always removed.
 *   */
/* Persist one bring-up record. A hard power-off during a bring-up experiment
 * loses whatever is still in the hive's dirty pages, so every record is
 * flushed (ZwFlushKey) and a monotonic Step marker is written first: if the
 * machine dies on a register access, the last Step tells exactly where. */
/* The mirror must never truncate: a 64-entry cap silently dropped every record
 * after the 64th, which made "no entry after step N" look like a bus stall. */
#define TG_DUMP_MAX 192
#define TG_DUMP_NAME 24
static struct { CHAR name[TG_DUMP_NAME]; UINT32 value; } g_dump[TG_DUMP_MAX];
static UINT32 g_dump_count;

static VOID write_dump_file(VOID)
{
    UNICODE_STRING path;
    OBJECT_ATTRIBUTES attributes;
    IO_STATUS_BLOCK iosb;
    HANDLE handle = NULL;
    /* Static, not stack: 6 KiB would be a large slice of the kernel stack. The
     * mirror is only produced from the single-threaded bring-up path. */
    static CHAR text[TG_DUMP_MAX * 32 + 64];
    UINT32 length = 0, entry;
    static const CHAR hex[] = "0123456789ABCDEF";
    if (KeGetCurrentIrql() > PASSIVE_LEVEL)
        return;
    for (entry = 0; entry < g_dump_count; ++entry) {
        UINT32 index = 0, nibble;
        while (g_dump[entry].name[index] != 0 && length + 32 < sizeof(text))
            text[length++] = g_dump[entry].name[index++];
        while (index < 20 && length + 32 < sizeof(text)) { text[length++] = ' '; ++index; }
        text[length++] = '0'; text[length++] = 'x';
        for (nibble = 8; nibble != 0; --nibble) {
            UINT32 shift = (nibble - 1u) * 4u;
            text[length++] = hex[(g_dump[entry].value >> shift) & 0xFu];
        }
        text[length++] = '\r'; text[length++] = '\n';
    }
    if (length == 0)
        return;
    RtlInitUnicodeString(&path, L"\\??\\C:\\SwitchWOA\\gpu-run.txt");
    InitializeObjectAttributes(&attributes, &path,
        OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (!NT_SUCCESS(ZwCreateFile(&handle,
            FILE_WRITE_DATA | SYNCHRONIZE, &attributes, &iosb, NULL,
            FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE,
            FILE_OVERWRITE_IF,
            FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE | FILE_WRITE_THROUGH,
            NULL, 0)))
        return;
    (VOID)ZwWriteFile(handle, NULL, NULL, NULL, &iosb, text, length, NULL, NULL);
    (VOID)ZwClose(handle);
}

static void record32(const WCHAR *name, uint32_t value)
{
    static HANDLE key = NULL;
    UNICODE_STRING path, value_name;
    OBJECT_ATTRIBUTES attributes;
    if (key == (HANDLE)-1)
        return;
    if (key == NULL) {
        if (KeGetCurrentIrql() > PASSIVE_LEVEL) { key = (HANDLE)-1; return; }
        RtlInitUnicodeString(&path,
            L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\tegragpurender\\Parameters");
        InitializeObjectAttributes(&attributes, &path,
            OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
        if (!NT_SUCCESS(ZwOpenKey(&key, KEY_SET_VALUE | KEY_QUERY_VALUE, &attributes))) {
            key = (HANDLE)-1;
            return;
        }
    }
    RtlInitUnicodeString(&value_name, name);
    if (g_dump_count < TG_DUMP_MAX) {
        UINT32 index = 0;
        while (name[index] != 0 && index < TG_DUMP_NAME - 1u) {
            g_dump[g_dump_count].name[index] = (CHAR)(name[index] & 0x7F);
            ++index;
        }
        g_dump[g_dump_count].name[index] = 0;
        g_dump[g_dump_count].value = value;
        ++g_dump_count;
    }
    if (NT_SUCCESS(ZwSetValueKey(key, &value_name, 0, REG_DWORD, &value, sizeof(value))) &&
        KeGetCurrentIrql() == PASSIVE_LEVEL)
        (void)ZwFlushKey(key);
}
static void record_step(uint32_t step)
{
    record32(L"Step", step);
    write_dump_file();   /* live mirror: survives a bus stall or a hard power-off */
}

/* Exposed so t210_clock.c can mark each PLL sub-step in the same mirror. */
void tg_gpu_clock_mark(const struct tg_gpu_power_io *io, uint32_t step)
{
    (void)io;
    record_step(step);
}

/* Let the clock unit persist a few raw readings under fixed names. */
void tg_gpu_clock_note(const struct tg_gpu_power_io *io, uint32_t slot, uint32_t value)
{
    static const WCHAR *names[8] = { L"ClkNote0", L"ClkNote1", L"ClkNote2", L"ClkNote3",
                                     L"ClkNote4", L"ClkNote5", L"ClkNote6", L"ClkNote7" };
    (void)io;
    if (slot < 8u)
        record32(names[slot], value);
}
static uint32_t swt210_rg_write(void)
{
    RTL_QUERY_REGISTRY_TABLE table[2] = { 0 };
    /* Default 1 = write 0 to PMC+0x2D4 once the rail is up. Verified on hardware:
     * with VDD_GPU enabled (MAX77620 GPIO6 high + MAX77621 enable) the write
     * returns and BAR0 answers NV_PMC_BOOT_0 = 0x12B000A1. With the rail off the
     * same write stalls the APB, which is what made it look fatal before. */
    uint32_t value = 1;
    table[0].Flags = RTL_QUERY_REGISTRY_DIRECT;
    table[0].Name = L"RgWrite";
    table[0].EntryContext = &value;
    table[0].DefaultType = REG_DWORD;
    table[0].DefaultData = &value;
    table[0].DefaultLength = sizeof(value);
    (void)RtlQueryRegistryValues(RTL_REGISTRY_SERVICES,
                                 L"tegragpurender\\Parameters", table, NULL, NULL);
    return value;
}

static uint32_t swt210_rail_probe(void)
{
    RTL_QUERY_REGISTRY_TABLE table[2] = { 0 };
    uint32_t value = 0;
    table[0].Flags = RTL_QUERY_REGISTRY_DIRECT;
    table[0].Name = L"RailProbe";
    table[0].EntryContext = &value;
    table[0].DefaultType = REG_DWORD;
    table[0].DefaultData = &value;
    table[0].DefaultLength = sizeof(value);
    (void)RtlQueryRegistryValues(RTL_REGISTRY_SERVICES,
                                 L"tegragpurender\\Parameters", table, NULL, NULL);
    return value;
}

static uint32_t swt210_aperture_probe(void)
{
    RTL_QUERY_REGISTRY_TABLE table[2] = { 0 };
    uint32_t value = 0;
    table[0].Flags = RTL_QUERY_REGISTRY_DIRECT;
    table[0].Name = L"ApertureProbe";
    table[0].EntryContext = &value;
    table[0].DefaultType = REG_DWORD;
    table[0].DefaultData = &value;
    table[0].DefaultLength = sizeof(value);
    (void)RtlQueryRegistryValues(RTL_REGISTRY_SERVICES,
                                 L"tegragpurender\\Parameters", table, NULL, NULL);
    return value;
}

static uint32_t swt210_rail_sweep(void)
{
    RTL_QUERY_REGISTRY_TABLE table[2] = { 0 };
    uint32_t value = 0;
    table[0].Flags = RTL_QUERY_REGISTRY_DIRECT;
    table[0].Name = L"RailSweep";
    table[0].EntryContext = &value;
    table[0].DefaultType = REG_DWORD;
    table[0].DefaultData = &value;
    table[0].DefaultLength = sizeof(value);
    (void)RtlQueryRegistryValues(RTL_REGISTRY_SERVICES,
                                 L"tegragpurender\\Parameters", table, NULL, NULL);
    return value;
}

static uint32_t swt210_pri_ring(void)
{
    RTL_QUERY_REGISTRY_TABLE table[2] = { 0 };
    uint32_t value = 1;   /* default: initialize the priv ring (nvgpu order) */
    table[0].Flags = RTL_QUERY_REGISTRY_DIRECT;
    table[0].Name = L"PriRing";
    table[0].EntryContext = &value;
    table[0].DefaultType = REG_DWORD;
    table[0].DefaultData = &value;
    table[0].DefaultLength = sizeof(value);
    (void)RtlQueryRegistryValues(RTL_REGISTRY_SERVICES,
                                 L"tegragpurender\\Parameters", table, NULL, NULL);
    return value;
}

/* nvgpu gm20b_priv_ring_enable(): without this the SYS/GPC register space,
 * including the whole GPCPLL page and the GPC broadcast page, answers with
 * priv-ring BADF codes (0xBADF3000/0xBADF5000) while PMC, PRIV_RING and LTC
 * still respond. The frontend-start path already wrote 0x12004c/0x122204, but
 * that path is skipped by PowerSkipMask bit 7, so the ring never came up. */
static void priv_ring_init(const struct tg_gpu_power_io *io)
{
    io->write32(io->context, 0x570120A8u, 0u);   /* slcg_priring prod value */
    (void)io->read32(io->context, 0x570120A8u);
    io->write32(io->context, 0x5712004Cu, 4u);   /* pri_ringmaster_command */
    (void)io->read32(io->context, 0x5712004Cu);
    io->write32(io->context, 0x57122204u, 2u);   /* pri_ringstation sys decode */
    (void)io->read32(io->context, 0x57122204u);
}

static uint32_t swt210_rail_settle_ms(void)
{
    RTL_QUERY_REGISTRY_TABLE table[2] = { 0 };
    /* Default 25 ms: the rail-gate release (PMC+0x2D4=0) makes the PMC wait for
     * VDD_GPU power-good, so it must not run while the MAX77621 is still
     * ramping. 5 ms was observed to be intermittent: one boot reached the
     * desktop, later boots stalled inside the same sequence. */
    uint32_t value = 25;
    table[0].Flags = RTL_QUERY_REGISTRY_DIRECT;
    table[0].Name = L"RailSettleMs";
    table[0].EntryContext = &value;
    table[0].DefaultType = REG_DWORD;
    table[0].DefaultData = &value;
    table[0].DefaultLength = sizeof(value);
    (void)RtlQueryRegistryValues(RTL_REGISTRY_SERVICES,
                                 L"tegragpurender\\Parameters", table, NULL, NULL);
    return value > 500u ? 500u : value;
}

static uint32_t swt210_toggle_probe(void)
{
    RTL_QUERY_REGISTRY_TABLE table[2] = { 0 };
    uint32_t value = 0;
    table[0].Flags = RTL_QUERY_REGISTRY_DIRECT;
    table[0].Name = L"ToggleProbe";
    table[0].EntryContext = &value;
    table[0].DefaultType = REG_DWORD;
    table[0].DefaultData = &value;
    table[0].DefaultLength = sizeof(value);
    (void)RtlQueryRegistryValues(RTL_REGISTRY_SERVICES,
                                 L"tegragpurender\\Parameters", table, NULL, NULL);
    return value;
}

static uint32_t swt210_clamp_needs_td(void)
{
    RTL_QUERY_REGISTRY_TABLE table[2] = { 0 };
    /* Default 0: do NOT gate the rail-gate release on PWRGATE_STATUS bit 1.
     * Measured on hardware, that bit stays 0 for the TD partition even when the
     * GPU answers its aperture, and L4T's tegra_powergate_sequence_power_up()
     * never waits on it either. Gating on it is what kept the GPU clamped. */
    uint32_t value = 0;
    table[0].Flags = RTL_QUERY_REGISTRY_DIRECT;
    table[0].Name = L"ClampNeedsTd";
    table[0].EntryContext = &value;
    table[0].DefaultType = REG_DWORD;
    table[0].DefaultData = &value;
    table[0].DefaultLength = sizeof(value);
    (void)RtlQueryRegistryValues(RTL_REGISTRY_SERVICES,
                                 L"tegragpurender\\Parameters", table, NULL, NULL);
    return value;
}

static uint32_t swt210_require_td_status(void)
{
    RTL_QUERY_REGISTRY_TABLE table[2] = { 0 };
    uint32_t value = 0;
    table[0].Flags = RTL_QUERY_REGISTRY_DIRECT;
    table[0].Name = L"RequireTdStatus";
    table[0].EntryContext = &value;
    table[0].DefaultType = REG_DWORD;
    table[0].DefaultData = &value;
    table[0].DefaultLength = sizeof(value);
    (void)RtlQueryRegistryValues(RTL_REGISTRY_SERVICES,
                                 L"tegragpurender\\Parameters", table, NULL, NULL);
    return value;
}

static uint32_t swt210_power_skip_mask(void)
{
    static uint32_t cached = UINT32_MAX;
    RTL_QUERY_REGISTRY_TABLE table[2] = { 0 };
    uint32_t value = 0;
    if (cached != UINT32_MAX)
        return cached;
    table[0].Flags = RTL_QUERY_REGISTRY_DIRECT;
    table[0].Name = L"PowerSkipMask";
    table[0].EntryContext = &value;
    table[0].DefaultType = REG_DWORD;
    table[0].DefaultData = &value;
    table[0].DefaultLength = sizeof(value);
    (void)RtlQueryRegistryValues(RTL_REGISTRY_SERVICES,
                                 L"tegragpurender\\Parameters", table, NULL, NULL);
    return value;
}

#define CAR_BASE        0x60006000u
#define CAR_PLLP        (CAR_BASE + 0x0a0u)
/* Tegra210 CAR orders ids as L/H/U (0x10-0x18), V/W (0x1c-0x20), X (0x280) for
 * ids 160-191 and Y (0x298) for ids 192-223: the u-boot derived T210 driver
 * lists GPU as entry 184 in the X bank, i.e. X-bank bit 24. Verified on hardware:
 * writing CLK_ENB_X_SET bit 24 reads back set, and RESET_X bit 24 controls the
 * engine reset. */
#define CAR_CLOCK_X     (CAR_BASE + 0x280u)
#define CAR_CLOCK_SET   (CAR_BASE + 0x284u)
#define CAR_CLOCK_CLR   (CAR_BASE + 0x288u)
#define CAR_RESET_X     (CAR_BASE + 0x28cu)
#define CAR_RESET_SET   (CAR_BASE + 0x290u)
#define CAR_RESET_CLR   (CAR_BASE + 0x294u)
#define CAR_PLLP_OUT5   (CAR_BASE + 0x67cu)
#define GPU_GATE        (1u << 24) /* TEGRA210_CLK_GPU 184 -> X bit 24 */
#define GPU_REF         (1u << 29) /* TEGRA210_CLK_PLL_G_REF 189 -> X bit 29 */
#define GPU_CLOCKS      (GPU_GATE | GPU_REF)
#define OUT5_MASK       0xff070000u
/* 408 MHz * 2 / (2 + 2) = 204 MHz; retain enable/reset, override bit18. */
#define OUT5_204MHZ     0x02070000u
/* Tegra210 PMC has no GPU partition: the 3D engine is the TD partition
 * (POWER_PARTID_TD = 1). Clamp state is PMC+0x2C; the remove-clamping
 * command is PMC+0x34; both use a per-partition bit. The old 0x7000E6D4
 * is the Tegra214/B01 offset and hits an unrelated register on T210. */
#define PMC_CLAMP_STATUS 0x7000E42Cu
#define PMC_PWRGATE_TOGGLE 0x7000E430u
#define PMC_PWRGATE_STATUS 0x7000E438u
#define PMC_CLAMP_CLEAR  0x7000E434u
#define PMC_CTRL         0x7000E400u
#define PMC_SEC_DISABLE  0x7000E404u
#define PMC_PWRGATE_TIMER_OFF 0x7000E428u
#define PMC_PWRGOOD_TIMER 0x7000E43Cu
#define PMC_VIC_PARTID   23u
/* Tegra124+ GPU clamps are NOT the generic per-partition clamp bit: they live
 * in PMC+0x2D4 (GPU_RG_CNTRL) and writing 0 removes them. See
 * drivers/soc/tegra/pmc.c (has_gpu_clamps) in the Switchroot kernel. */
#define PMC_GPU_RG_CNTRL 0x7000E6D4u
#define PMC_PWR_DET      0x7000E448u
#define PMC_PG_MASK      0x7000E4D0u
#define PMC_PG_MASK_1    0x7000E4D4u
#define PMC_TD_PG_TIMER  0x7000E648u
#define PMC_SEC_DISABLE2 0x7000E690u
#define PMC_PG_MASK_ANDOR 0x7000E69Cu
#define PMC_PWR_DET_LATCH 0x7000E44Cu
#define PMC_PWR_DET_VAL   0x7000E4E4u
#define PMC_SCRATCH0      0x7000E450u
#define PMC_SCRATCH1      0x7000E454u
#define PMC_SCRATCH2      0x7000E458u
#define GPU_PARTID_BIT   (1u << 1)
#define MC_FLUSH        0x70019970u
#define MC_FLUSH_STATUS 0x70019974u
#define MC_GPU_BIT      (1u << 2) /* MC client 34 in the second bank */

/* MAX77621 at I2C5 0x1C (Erista GPU rail). */
#define MAX77620_ADDR       0x3Cu
#define MAX77620_AME_GPIO   0x40u
#define MAX77620_REG_GPIO6  0x3Cu
#define MAX77620_GPIO6_HIGH 0x09u  /* output, value high, push-pull */
#define MAX77621_GPU_ADDR   0x1Cu
#define MAX77621_VOUT       0x00u
#define MAX77621_VOUT_DVC   0x01u
#define MAX77621_ENABLE     0x80u
#define MAX77621_MIN_UV     606250u
#define MAX77621_STEP_UV    6250u
#define MAX77621_BRINGUP_MV 800u   /* 800 mV bring-up; DVFS raises this later */

static uint8_t max77621_code(uint32_t mv)
{
    uint32_t uv = mv * 1000u;
    if (uv <= MAX77621_MIN_UV) return 0u;
    uv = (uv - MAX77621_MIN_UV + MAX77621_STEP_UV - 1u) / MAX77621_STEP_UV;
    return (uint8_t)(uv > 0x7Fu ? 0x7Fu : uv);
}

static void update32(const struct tg_gpu_power_io *io, uint32_t address,
                     uint32_t mask, uint32_t value)
{
    uint32_t old = io->read32(io->context, address);
    io->write32(io->context, address, (old & ~mask) | (value & mask));
    (void)io->read32(io->context, address);
}

static bool write_pmic_checked(const struct tg_gpu_power_io *io, uint8_t address,
                               uint8_t reg, uint8_t value)
{
    uint8_t readback;
    return io->write_pmic(io->context, address, reg, value) &&
           io->read_pmic(io->context, address, reg, &readback) && readback == value;
}

static bool write_max77621(const struct tg_gpu_power_io *io, uint8_t address,
                           uint8_t code, bool enable)
{
    uint8_t value = (uint8_t)((enable ? MAX77621_ENABLE : 0u) | (code & 0x7Fu));
    if (MAX77621_VOUT_DVC != MAX77621_VOUT) {
        if (!write_pmic_checked(io, address, MAX77621_VOUT_DVC, value))
            return false;
    }
    return write_pmic_checked(io, address, MAX77621_VOUT, value);
}

static bool read_max77621(const struct tg_gpu_power_io *io, uint8_t address,
                          uint8_t reg, uint8_t *value)
{
    return io->read_pmic(io->context, address, reg, value);
}

static bool drain_mc(const struct tg_gpu_power_io *io)
{
    unsigned int attempt, sample;
    uint32_t request;
    update32(io, MC_FLUSH, MC_GPU_BIT, MC_GPU_BIT);
    request = io->read32(io->context, MC_FLUSH);
    if (request == UINT32_MAX || !(request & MC_GPU_BIT)) return false;
    for (attempt = 0; attempt < 100000; ++attempt) {
        uint32_t value;
        io->delay_us(io->context, 10);
        value = io->read32(io->context, MC_FLUSH_STATUS);
        if (value == UINT32_MAX) return false;
        if (!(value & MC_GPU_BIT))
            continue;
        for (sample = 0; sample < 5; ++sample)
            if (io->read32(io->context, MC_FLUSH_STATUS) != value)
                break;
        if (sample == 5)
            return true;
    }
    return false;
}

/* Request one PMC power-gate change. Writing START|id toggles the partition;
 * the START bit self-clears once the hardware accepted the request. Returns
 * the number of 10 us polls until that clear (0xFFFFFFFF on timeout) and
 * stores the PWRGATE_STATUS seen afterwards in *status. */
static uint32_t pmc_toggle(const struct tg_gpu_power_io *io, uint32_t partid,
                           uint32_t wait_us, uint32_t *status)
{
    uint32_t spins = 0, total = wait_us / 10u + 1u;
    io->write32(io->context, PMC_PWRGATE_TOGGLE, (1u << 8) | (partid & 0x1Fu));
    while (spins < total &&
           (io->read32(io->context, PMC_PWRGATE_TOGGLE) & (1u << 8)) != 0) {
        io->delay_us(io->context, 10);
        ++spins;
    }
    if (status != NULL)
        *status = io->read32(io->context, PMC_PWRGATE_STATUS);
    return spins;
}

enum tg_gpu_power_result
tg_gpu_power_up(const struct tg_gpu_power_io *io, uint8_t pmic_address,
                struct tg_gpu_power *state)
{
    uint32_t pllp;
    uint8_t vout = 0;
    if (!io || !state || !io->read32 || !io->write32 || !io->read_pmic ||
        !io->write_pmic || !io->delay_us || pmic_address != MAX77621_GPU_ADDR)
        return TG_GPU_POWER_BAD_ARGUMENT;
    if (state->owned)
        return TG_GPU_POWER_BUSY;
    record_step(1);
    state->stage = TG_GPU_STAGE_SNAPSHOT;
    /* Erista reports 0x211x; T210B01 reports 0x212x. */
    if ((io->read32(io->context, 0x70000804u) & 0xfff0u) != 0x2110u)
        return TG_GPU_POWER_UNSUPPORTED_BOARD;
    {
        uint32_t hidrev = io->read32(io->context, 0x70000804u);
        uint32_t rsta = io->read32(io->context, CAR_RESET_X);
        uint32_t clampb = io->read32(io->context, PMC_CLAMP_STATUS);
        record32(L"BootHidrev", hidrev);
        record32(L"BootRstX", rsta);
        record32(L"BootClamp", clampb);
        state->original_gpu_rg = io->read32(io->context, PMC_GPU_RG_CNTRL);
        record32(L"BootGpuRg", state->original_gpu_rg);
        /* A warm reboot does not reset the GPU partition, so do not require the
         * engine to already be in reset: assert it here for a known start. */
        if (!(rsta & GPU_GATE)) {
            io->write32(io->context, CAR_RESET_SET, GPU_GATE);
            io->delay_us(io->context, 10);
            record32(L"RstAsserted", io->read32(io->context, CAR_RESET_X));
        }
    }
    record_step(2);
    pllp = io->read32(io->context, CAR_PLLP);
    if ((pllp & ((1u << 31) | (1u << 30) | (1u << 27))) != ((1u << 30) | (1u << 27)))
        return TG_GPU_POWER_CLOCK_ERROR;
    state->pmic_address = pmic_address;
    if (!read_max77621(io, pmic_address, MAX77621_VOUT, &vout))
        return TG_GPU_POWER_PMIC_ERROR;
    state->original_enable = (uint8_t)(vout & MAX77621_ENABLE);
    state->original_voltage = (uint8_t)(vout & 0x7Fu);
    state->original_clocks = io->read32(io->context, CAR_CLOCK_X);
    state->original_pllp_out5 = io->read32(io->context, CAR_PLLP_OUT5);
    state->original_mc_flush = io->read32(io->context, MC_FLUSH);
    state->owned = true;
    state->ready = false;
    state->stage = TG_GPU_STAGE_RAIL;
    /* Raise the rail before ungating the engine: 800 mV bring-up. */
    /* VDD_GPU is gated by MAX77620 GPIO6 (Hekate: "5: CPU PMIC, 6: GPU PMIC"),
     * which Hekate parks as an input so the rail is off. Nothing can power the
     * TD partition while the rail is down, so drive the pin before touching the
     * MAX77621 voltage. Both registers are reached through switchfan's bridge by
     * addressing 0x3c. */
    {
        uint8_t ame = 0, gpio6 = 0, readback = 0;
        /* Pre-state of the rail chips: the GPU MAX77621 (0x1C) control and ID
         * registers were never captured, so there is no evidence yet whether the
         * chip is programmed (CTRL1/CTRL2) or merely holding a VOUT byte. */
        {
            /* Only the registers the switchfan PMIC bridge implements. Reading
             * 0x02..0x07 / 0x3E / 0x3F returns INVALID_PARAMETER and the platform
             * layer latches the first failure into IoStatus, which then failed the
             * whole adapter start even though the GPU bring-up had succeeded. */
            uint8_t value = 0;
            if (io->read_pmic(io->context, MAX77621_GPU_ADDR, 0x00u, &value))
                record32(L"PreVout", value);
            if (io->read_pmic(io->context, MAX77621_GPU_ADDR, 0x01u, &value))
                record32(L"PreVoutDvc", value);
            if (io->read_pmic(io->context, MAX77621_GPU_ADDR, 0x06u, &value))
                record32(L"PreReg6", value);
            if (io->read_pmic(io->context, MAX77620_ADDR, MAX77620_REG_GPIO6, &value))
                record32(L"Gpio6Before", value);
        }
        if (!io->read_pmic(io->context, MAX77620_ADDR, MAX77620_AME_GPIO, &ame)) {
            record32(L"RailEnableReadStatus", UINT32_MAX);
        } else {
            record32(L"RailAmeBefore", ame);
            if (!io->write_pmic(io->context, MAX77620_ADDR, MAX77620_AME_GPIO,
                                (uint8_t)(ame & ~(1u << 6)))) {
                record32(L"RailAmeWriteStatus", UINT32_MAX);
            } else if (!io->read_pmic(io->context, MAX77620_ADDR, MAX77620_AME_GPIO, &readback)) {
                record32(L"RailAmeWriteStatus", UINT32_MAX - 1u);
            } else {
                record32(L"RailAmeAfter", readback);
            }
            if (!io->write_pmic(io->context, MAX77620_ADDR, MAX77620_REG_GPIO6,
                                MAX77620_GPIO6_HIGH)) {
                record32(L"RailGpioWriteStatus", UINT32_MAX);
            } else if (!io->read_pmic(io->context, MAX77620_ADDR, MAX77620_REG_GPIO6, &gpio6)) {
                record32(L"RailGpioReadStatus", UINT32_MAX);
            } else {
                record32(L"RailGpio6", gpio6);
            }
            /* Probe: drive the enable low and high again while watching PMC power
             * detect, so we know whether this pin really gates VDD_GPU and whether
             * any PWR_DET bit is wired to it. */
            /* MAX77620 GPIO snapshot: if the board routes the GPU rail's power
             * good to a GPIO input, driving the enable shows up here. */
            {
                static const WCHAR *names[8] = { L"Gpio0", L"Gpio1", L"Gpio2", L"Gpio3",
                                                 L"Gpio4", L"Gpio5", L"Gpio6b", L"Gpio7" };
                uint32_t i;
                for (i = 0; i < 8u; ++i) {
                    uint8_t reg = (uint8_t)(0x36u + i), value = 0;
                    if (io->read_pmic(io->context, MAX77620_ADDR, reg, &value))
                        record32(names[i], value);
                }
            }
            if (swt210_rail_probe()) {
                uint8_t in0 = 0, in1 = 0, in7 = 0;
                record32(L"PwrDetAtHigh", io->read32(io->context, PMC_PWR_DET_VAL));
                if (io->read_pmic(io->context, MAX77620_ADDR, 0x36u, &in0))
                    record32(L"In0High", in0);
                if (io->read_pmic(io->context, MAX77620_ADDR, 0x37u, &in1))
                    record32(L"In1High", in1);
                if (io->read_pmic(io->context, MAX77620_ADDR, 0x3Du, &in7))
                    record32(L"In7High", in7);

                (void)io->write_pmic(io->context, MAX77620_ADDR, MAX77620_REG_GPIO6, 0x00u);
                io->delay_us(io->context, 50000);
                record32(L"PwrDetAtLow", io->read32(io->context, PMC_PWR_DET_VAL));
                if (io->read_pmic(io->context, MAX77620_ADDR, 0x36u, &in0))
                    record32(L"In0Low", in0);
                if (io->read_pmic(io->context, MAX77620_ADDR, 0x37u, &in1))
                    record32(L"In1Low", in1);
                if (io->read_pmic(io->context, MAX77620_ADDR, 0x3Du, &in7))
                    record32(L"In7Low", in7);

                (void)io->write_pmic(io->context, MAX77620_ADDR, MAX77620_REG_GPIO6,
                                     MAX77620_GPIO6_HIGH);
                io->delay_us(io->context, 50000);
                record32(L"PwrDetAtHigh2", io->read32(io->context, PMC_PWR_DET_VAL));
                if (io->read_pmic(io->context, MAX77620_ADDR, 0x36u, &in0))
                    record32(L"In0High2", in0);
                if (io->read_pmic(io->context, MAX77620_ADDR, 0x37u, &in1))
                    record32(L"In1High2", in1);
                if (io->read_pmic(io->context, MAX77620_ADDR, 0x3Du, &in7))
                    record32(L"In7High2", in7);
                record32(L"PwrDetLatchProbe", io->read32(io->context, PMC_PWR_DET_LATCH));
            }
        }
    }
    record32(L"UefiMagic", io->read32(io->context, PMC_SCRATCH0));
    record32(L"UefiGpioBefore", io->read32(io->context, PMC_SCRATCH1));
    record32(L"UefiGpioAfter", io->read32(io->context, PMC_SCRATCH2));
    record32(L"PwrDetValBefore", io->read32(io->context, PMC_PWR_DET_VAL));
    if (!(swt210_power_skip_mask() & 1u) &&
        !write_max77621(io, pmic_address, max77621_code(MAX77621_BRINGUP_MV), true))
        return TG_GPU_POWER_PMIC_ERROR;
    {
        uint8_t vout_now = 0, chip1 = 0;
        if (read_max77621(io, pmic_address, MAX77621_VOUT, &vout_now))
            record32(L"RailVoutRead", vout_now);
        if (read_max77621(io, pmic_address, MAX77621_VOUT_DVC, &chip1))
            record32(L"RailVoutDvcRead", chip1);
        record32(L"PwrDetValAfter", io->read32(io->context, PMC_PWR_DET_VAL));
        record32(L"PwrDetLatchAfter", io->read32(io->context, PMC_PWR_DET_LATCH));
    }
    record_step(3);
    io->delay_us(io->context, 5000);
    record_step(4);

    /* T210: the GPU is the TD partition (id 1). Its rail-gate clamp (PMC+0x2D4,
     * GPU_RG_CNTRL) is asserted at boot, and while it is asserted the partition
     * neither powers on nor answers aperture accesses, so clear it first. */
    record_step(5);

    record32(L"RgWriteMode", swt210_rg_write());
    record32(L"GpuRgBeforeClear", io->read32(io->context, PMC_GPU_RG_CNTRL));
    {
        uint32_t st = io->read32(io->context, PMC_PWRGATE_STATUS);
        record32(L"PwrGateBefore", st);
        record_step(9);
    }
    if (!(swt210_power_skip_mask() & 0x40u)) {
        uint32_t st, ack, vic_off, vic_on, final;
        record32(L"PmcCtrl", io->read32(io->context, PMC_CTRL));
        record32(L"PmcSecDisable", io->read32(io->context, PMC_SEC_DISABLE));
        record32(L"PmcPwrGateTimerOff", io->read32(io->context, PMC_PWRGATE_TIMER_OFF));
        record32(L"PmcPwrGoodTimer", io->read32(io->context, PMC_PWRGOOD_TIMER));
        record32(L"PmcPwrDet", io->read32(io->context, PMC_PWR_DET));
        record32(L"PmcPgMask", io->read32(io->context, PMC_PG_MASK));
        record32(L"PmcPgMask1", io->read32(io->context, PMC_PG_MASK_1));
        record32(L"PmcTdPgTimer", io->read32(io->context, PMC_TD_PG_TIMER));
        record32(L"PmcSecDisable2", io->read32(io->context, PMC_SEC_DISABLE2));
        record32(L"PmcPgMaskAndor", io->read32(io->context, PMC_PG_MASK_ANDOR));
        st = io->read32(io->context, PMC_PWRGATE_STATUS);
        record32(L"PwrGateBefore", st);
        record32(L"TglBefore", io->read32(io->context, PMC_PWRGATE_TOGGLE));
        record32(L"VicWasOn", ((st & (1u << PMC_VIC_PARTID)) != 0) ? 1u : 0u);
        record_step(10);
        /* Control: prove this context can move a partition at all. Two toggles
         * of the unused VIC partition restore the boot state. */
        ack = pmc_toggle(io, PMC_VIC_PARTID, 50000u, &vic_off);
        record32(L"VicToggle1Ack", ack);
        record32(L"VicToggle1Status", vic_off);
        if (ack != 0xFFFFFFFFu) {
            ack = pmc_toggle(io, PMC_VIC_PARTID, 50000u, &vic_on);
            record32(L"VicToggle2Ack", ack);
            record32(L"VicToggle2Status", vic_on);
        } else {
            record32(L"VicToggle2Ack", 0xFFFFFFFFu);
            record32(L"VicToggle2Status", 0u);
        }
        record_step(11);
        /* The GPU itself: TD partition, POWER_PARTID_TD = 1. */
        if (!(st & GPU_PARTID_BIT)) {
            ack = pmc_toggle(io, 1u, 25000u, &st);
            record_step(12);
            record32(L"TdTglAck", ack);
            record32(L"TdStatus1", st);
            io->delay_us(io->context, 200000);
            st = io->read32(io->context, PMC_PWRGATE_STATUS);
            record32(L"TdStatus1b", st);
            record_step(13);
            if (!(st & GPU_PARTID_BIT)) {
                record32(L"TdClampBefore", io->read32(io->context, PMC_CLAMP_STATUS));
                io->write32(io->context, PMC_CLAMP_CLEAR, GPU_PARTID_BIT);
                io->delay_us(io->context, 10000);
                record32(L"TdClampAfterClear", io->read32(io->context, PMC_CLAMP_STATUS));
                ack = pmc_toggle(io, 1u, 100000u, &st);
                record32(L"TdTglAck2", ack);
                record32(L"TdStatus2", st);
                io->delay_us(io->context, 200000);
                st = io->read32(io->context, PMC_PWRGATE_STATUS);
                record32(L"TdStatus2b", st);
            }
        }
        final = io->read32(io->context, PMC_PWRGATE_STATUS);
        record32(L"PwrGateAfter", final);
        record_step(14);
        record32(L"TdStatusFinal", final);
        /* The partition never reports powered at the 800 mV bring-up rail; the
         * power switch only closes when the core rail satisfies its power-good
         * comparator, so retry the toggle across plausible rail voltages. */
        if (!(final & GPU_PARTID_BIT) && swt210_rail_sweep()) {
            static const uint32_t sweep_mv[3] = { 900u, 1000u, 1100u };
            uint32_t index;
            for (index = 0; index < 3u; ++index) {
                WCHAR name[8];
                record_step(30u + index);
                if (!write_max77621(io, pmic_address,
                                    max77621_code(sweep_mv[index]), true))
                    break;
                io->delay_us(io->context, 5000);
                ack = pmc_toggle(io, 1u, 25000u, &st);
                io->delay_us(io->context, 20000);
                st = io->read32(io->context, PMC_PWRGATE_STATUS);
                name[0] = L'R'; name[1] = L'a'; name[2] = L'i'; name[3] = L'l';
                name[4] = (WCHAR)(L'0' + index); name[5] = 0;
                record32(name, st);
                final = st;
                if (st & GPU_PARTID_BIT)
                    break;
            }
            record32(L"TdRailMv", sweep_mv[index < 3u ? index : 2u]);
        }
        /* L4T's tegra_powergate_sequence_power_up() writes PWRGATE_TOGGLE and
         * never waits on PWRGATE_STATUS for the 3D partition, so the status bit
         * is recorded but only enforced when RequireTdStatus=1. */
        if (!(final & GPU_PARTID_BIT) && swt210_require_td_status())
            return TG_GPU_POWER_BUSY;
        if (io->read32(io->context, PMC_CLAMP_STATUS) & GPU_PARTID_BIT) {
            io->write32(io->context, PMC_CLAMP_CLEAR, GPU_PARTID_BIT);
            io->delay_us(io->context, 10);
        }
        record32(L"ClampAfterPowerGate", io->read32(io->context, PMC_CLAMP_STATUS));
        /* T210 exposes a second graphics partition, TEGRA210_POWER_DOMAIN_3D1 =
         * 13 (dt-bindings/soc/tegra210-powergate.h); partition 1 has no entry in
         * that table at all. Toggle it once and back to see whether the graphics
         * power domain this SoC actually reports as powerable is 13, not 1. */
        if (swt210_toggle_probe()) {
            record_step(25);
            ack = pmc_toggle(io, 13u, 25000u, &st);
            record32(L"Td1TglAck", ack);
            record32(L"Td1StatusOn", st);
            record32(L"Td1Clamp", io->read32(io->context, PMC_CLAMP_STATUS));
            record32(L"Td1PllCfgOn", io->read32(io->context, 0x57000000u + 0x137000u));
            record32(L"Td1RgOn", io->read32(io->context, PMC_GPU_RG_CNTRL));
            record_step(26);
            if (swt210_toggle_probe() == 1u) {
                /* Bisect mode: restore the partition so the boot state is unchanged. */
                ack = pmc_toggle(io, 13u, 25000u, &st);
                record32(L"Td1TglAck2", ack);
                record32(L"Td1StatusOff", st);
                record32(L"Td1RgAfter", io->read32(io->context, PMC_GPU_RG_CNTRL));
            } else {
                /* ToggleProbe=2 leaves 3D1 powered so the PLL page can be
                 * re-probed: TEGRA210_POWER_DOMAIN_3D1 is the only graphics
                 * domain L4T registers, and the GPCPLL page currently answers
                 * with priv-ring BADF errors. */
                record32(L"Td1LeftOn", st);
            }
            record_step(27);
        }
    }

    state->stage = TG_GPU_STAGE_CLOCKS;

    if (!(swt210_power_skip_mask() & 2u)) {
    record_step(20);
    record32(L"ClkXBeforeSet", io->read32(io->context, CAR_CLOCK_X));
    record32(L"RstXBeforeSet", io->read32(io->context, CAR_RESET_X));
    update32(io, CAR_PLLP_OUT5, OUT5_MASK, OUT5_204MHZ);
    io->delay_us(io->context, 2);
    record32(L"Out5Read", io->read32(io->context, CAR_PLLP_OUT5));
    if ((io->read32(io->context, CAR_PLLP_OUT5) & OUT5_MASK) != OUT5_204MHZ)
        return TG_GPU_POWER_CLOCK_ERROR;
    /* Enable TEGRA210_CLK_GPU (Y bit 24) and PLL_G_REF (Y bit 29) separately so a
     * single refused gate is visible instead of collapsing into one error. */
    io->write32(io->context, CAR_CLOCK_SET, GPU_GATE);
    io->delay_us(io->context, 2);
    record32(L"ClkXAfterGate", io->read32(io->context, CAR_CLOCK_X));
    io->write32(io->context, CAR_CLOCK_SET, GPU_REF);
    io->delay_us(io->context, 10);
    record_step(21);
    /* While the engine is held in reset the CAR may keep the gate readback low,
     * so this is recorded only; the reset stage re-checks both after release. */
    record32(L"ClkXAfterSet", io->read32(io->context, CAR_CLOCK_X));
    }

    state->stage = TG_GPU_STAGE_RESET;
    /* Linux order is power-on -> clock enable -> remove clamping -> reset
     * release. Removing the GPU rail-gate clamp while the engine has no clock
     * stalls the APB (measured: the write never returns), so it happens here,
     * after the CAR clock stage. */
    {
        uint32_t mode = swt210_rg_write();
        uint32_t gate = io->read32(io->context, PMC_PWRGATE_STATUS);
        record32(L"ClampGateStatus", gate);
        if (mode != 0u && ((gate & GPU_PARTID_BIT) || !swt210_clamp_needs_td())) {
            uint32_t settle = swt210_rail_settle_ms();
            uint32_t before = io->read32(io->context, PMC_GPU_RG_CNTRL);
            uint32_t value = 0u;
            if (mode == 2u) value = before & ~1u;
            else if (mode == 3u) value = before | 1u;
            /* Let VDD_GPU reach power-good before releasing the rail gate. */
            record32(L"RailSettleMs", settle);
            if (settle)
                io->delay_us(io->context, settle * 1000u);
            record32(L"RailVoutAtGate", 0u);
            { uint8_t vout_gate = 0;
              if (read_max77621(io, pmic_address, MAX77621_VOUT, &vout_gate))
                  record32(L"RailVoutAtGate", vout_gate); }
            record_step(23);   /* last marker before the GPU_RG_CNTRL write */
            io->write32(io->context, PMC_GPU_RG_CNTRL, value);
            record_step(24);   /* write returned */
            record32(L"RgWrote", value);
            io->delay_us(io->context, 20);
            record32(L"GpuRgCleared", io->read32(io->context, PMC_GPU_RG_CNTRL));
            /* If the rail-gate release really powers the GPU, the partition
             * status and the clamp state should move together with it. */
            record32(L"PwrGateAfterRg", io->read32(io->context, PMC_PWRGATE_STATUS));
            record32(L"ClampAfterRg", io->read32(io->context, PMC_CLAMP_STATUS));
            record32(L"RgAfterWrite", io->read32(io->context, PMC_GPU_RG_CNTRL));
            /* Do NOT touch the GPCPLL/GPC page here: measured, a read issued right
             * after the rail-gate release stalls the APB (the SYS/GPC fabric is
             * still coming up). The A/B probe runs after the MC drain instead. */
        }
    }
    if (!(swt210_power_skip_mask() & 4u)) {
    io->write32(io->context, CAR_RESET_SET, GPU_GATE);
    io->delay_us(io->context, 10);
    io->write32(io->context, PMC_CLAMP_CLEAR, GPU_PARTID_BIT);
    io->delay_us(io->context, 10);
    record32(L"ClampAfterClear", io->read32(io->context, PMC_CLAMP_STATUS));
    if ((io->read32(io->context, PMC_CLAMP_STATUS) & GPU_PARTID_BIT) != 0)
        return TG_GPU_POWER_RESET_ERROR;
    io->delay_us(io->context, 10);
    io->write32(io->context, CAR_CLOCK_CLR, GPU_GATE);
    io->write32(io->context, CAR_RESET_CLR, GPU_GATE);
    io->write32(io->context, CAR_CLOCK_SET, GPU_GATE);
    {
        uint32_t rst = io->read32(io->context, CAR_RESET_X);
        uint32_t clampv = io->read32(io->context, PMC_CLAMP_STATUS);
        uint32_t clk = io->read32(io->context, CAR_CLOCK_X);
        record32(L"RstX", rst);
        record32(L"ClampSt", clampv);
        record32(L"ClkX", clk);
        record32(L"ClkXFinal", io->read32(io->context, CAR_CLOCK_X));
        if ((rst & GPU_GATE) || (clampv & GPU_PARTID_BIT) ||
            (clk & GPU_CLOCKS) != GPU_CLOCKS)
            return TG_GPU_POWER_RESET_ERROR;
    }
    }

    state->stage = TG_GPU_STAGE_MC;
    if (!(swt210_power_skip_mask() & 8u) && !drain_mc(io))
        return TG_GPU_POWER_MC_TIMEOUT;
    io->delay_us(io->context, 10);
    if (!(swt210_power_skip_mask() & 0x10u))
        update32(io, MC_FLUSH, MC_GPU_BIT, 0);
    io->delay_us(io->context, 10);
    state->ready = true;
    state->stage = TG_GPU_STAGE_READY;
    /* The SYS/GPC register space needs the GPU's fabric settled before it answers;
     * earlier than this point a probe read stalls the APB. Wait, then measure the
     * A/B effect of the priv-ring enable (nvgpu gm20b_priv_ring_enable) on the
     * GPCPLL page, which was answering 0xBADF3000 in every earlier round. */
    io->delay_us(io->context, 20000);
    record_step(28);
    record32(L"PriCfgBefore", io->read32(io->context, 0x57137000u));
    record32(L"PriCoeffBefore", io->read32(io->context, 0x57137004u));
    if (swt210_pri_ring()) {
        priv_ring_init(io);
        io->delay_us(io->context, 3000);
        record32(L"PriRing", 1u);
        record32(L"PriCfgAfter", io->read32(io->context, 0x57137000u));
        record32(L"PriCoeffAfter", io->read32(io->context, 0x57137004u));
        record32(L"PriRingCmd", io->read32(io->context, 0x5712004Cu));
        record32(L"PriDecodeCfg", io->read32(io->context, 0x57122204u));
        record32(L"PriDvfs0After", io->read32(io->context, 0x57137010u));
        record32(L"PriBcastAfter", io->read32(io->context, 0x57132820u));
    } else {
        record32(L"PriRing", 0u);
    }
    record_step(29);
    /* Read-only aperture probe (ApertureProbe=1): does the GPU answer at all with
     * the partition gated and the rail-gate clamp still asserted? Runs last so a
     * bus stall cannot hide the records above. */
    if (swt210_aperture_probe()) {
        /* Read-only inventory: which GPU blocks answer at all after the rail-gate
         * release. 0x12B000A1 for BOOT_0 came back earlier while 0x137000 read
         * 0xBADF3000, which looks like NVIDIA's "gated/bad block" pattern; the
         * first clock-stage write then stalled the bus. */
        record_step(46);
        record32(L"InvPmcEnable", io->read32(io->context, 0x57000200u));
        record32(L"InvBusIntr", io->read32(io->context, 0x57001100u));
        record32(L"InvPriStat0", io->read32(io->context, 0x57120058u));
        record32(L"InvLtcMax", io->read32(io->context, 0x57022454u));
        record32(L"InvLtcCnt", io->read32(io->context, 0x5712006cu));
        record32(L"InvPllCfg", io->read32(io->context, 0x57137000u));
        record32(L"InvPllCoeff", io->read32(io->context, 0x57137004u));
        record32(L"InvPllVco", io->read32(io->context, 0x57137100u));
        record32(L"InvPllOut", io->read32(io->context, 0x57137250u));
        record32(L"InvSlow", io->read32(io->context, 0x57020160u));
        record32(L"InvSvop", io->read32(io->context, 0x57021944u));
        record32(L"InvTherm", io->read32(io->context, 0x57020798u));
        record32(L"InvDvfs2", io->read32(io->context, 0x57132820u));
        record32(L"InvCountVal", io->read32(io->context, 0x57134128u));
        record32(L"InvNdivSlow", io->read32(io->context, 0x5713701cu));
        record32(L"InvBypass", io->read32(io->context, 0x57137340u));
        record_step(49);
        /* Map which slots of the GPCPLL page answer at all: several read
         * 0xBADF3000, which is what corrupted the read-modify-write path. */
        {
            WCHAR name[8];
            uint32_t index;
            for (index = 0; index < 32u; ++index) {
                name[0] = L'P';
                name[1] = (WCHAR)(L'0' + index / 10u);
                name[2] = (WCHAR)(L'0' + index % 10u);
                name[3] = 0;
                record32(name, io->read32(io->context, 0x57137000u + index * 4u));
            }
            for (index = 0; index < 16u; ++index) {
                name[0] = L'G';
                name[1] = (WCHAR)(L'0' + index / 10u);
                name[2] = (WCHAR)(L'0' + index % 10u);
                name[3] = 0;
                record32(name, io->read32(io->context, 0x57132800u + index * 4u));
            }
        }
        record_step(50);
        /* Two discriminating probes, both restored immediately. Neither enables
         * the PLL nor switches to VCO, so this cannot reproduce the VCO stall:
         * 1) does enabling the GPU's internal MC (NV_PMC_ENABLE bit 8) make the
         *    GPCPLL page answer instead of returning the priv-ring BADF codes?
         * 2) are the BADF values stubbed reads (two different writes read back
         *    the same) or dropped writes? */
        record_step(51);
        {
            uint32_t enable = io->read32(io->context, 0x57000200u);
            if (enable != UINT32_MAX) {
                io->write32(io->context, 0x57000200u, enable | 0x100u);
                io->delay_us(io->context, 500);
                record32(L"McCfgAfter", io->read32(io->context, 0x57137000u));
                record32(L"McCoeffAfter", io->read32(io->context, 0x57137004u));
                record32(L"McEnableAfter", io->read32(io->context, 0x57000200u));
                io->write32(io->context, 0x57000200u, enable);
                record32(L"McEnableBack", io->read32(io->context, 0x57000200u));
            }
        }
        record_step(52);
        {
            uint32_t first, second;
            io->write32(io->context, 0x57137004u, 0x00012345u);
            first = io->read32(io->context, 0x57137004u);
            io->write32(io->context, 0x57137004u, 0x00054321u);
            second = io->read32(io->context, 0x57137004u);
            io->write32(io->context, 0x57137004u, 0u);
            record32(L"RwFirst", first);
            record32(L"RwSecond", second);
        }
        record_step(53);
        record_step(47);
        /* The AUX window (0x538F0000) reaches the GPU through a different
         * decoding path than BAR0, so it separates "GPU totally unresponsive"
         * from "this one aperture window is dead". */
        record32(L"Aux0", io->read32(io->context, 0x538F0000u));
        record_step(48);
        record32(L"Aux1", io->read32(io->context, 0x538F0010u));
        record_step(40);
        record32(L"Aperture0", io->read32(io->context, 0x57000000u));
        record_step(41);
        record32(L"Aperture1", io->read32(io->context, 0x57001000u));
        record_step(42);
        record32(L"AperturePllCfg", io->read32(io->context, 0x57000000u + 0x137000u));
        record_step(43);
        record32(L"AperturePllOut", io->read32(io->context, 0x57000000u + 0x137250u));
        record_step(44);
    }
    /* Terminal marker: the mirror is only flushed from record_step(), so without
     * this the file always appears to stop at the last step-gated operation even
     * when the whole sequence returned. */
    record_step(60);
    return TG_GPU_POWER_OK;
}

enum tg_gpu_power_result
tg_gpu_power_drain(const struct tg_gpu_power_io *io, const struct tg_gpu_power *state)
{
    if (!state->owned || !state->ready) return TG_GPU_POWER_BAD_ARGUMENT;
    return drain_mc(io) ? TG_GPU_POWER_OK : TG_GPU_POWER_MC_TIMEOUT;
}

enum tg_gpu_power_result
tg_gpu_power_down(const struct tg_gpu_power_io *io, struct tg_gpu_power *state)
{
    bool ok;
    uint32_t clamp, reset;
    if (!state->owned)
        return TG_GPU_POWER_OK;
    uint32_t skip = swt210_power_skip_mask();
    if (!(skip & 8u) && state->stage >= TG_GPU_STAGE_MC && !drain_mc(io))
        return TG_GPU_POWER_MC_TIMEOUT;
    if (!(skip & 4u)) {
    /* T210: no clamp-assert command; the CAR reset isolates the engine. */
        io->delay_us(io->context, 10);
    clamp = io->read32(io->context, PMC_CLAMP_STATUS);
    if (clamp == UINT32_MAX) return TG_GPU_POWER_RESET_ERROR;
        io->write32(io->context, CAR_RESET_SET, GPU_GATE);
        io->delay_us(io->context, 10);
        reset = io->read32(io->context, CAR_RESET_X);
        if (reset == UINT32_MAX || !(reset & GPU_GATE)) return TG_GPU_POWER_RESET_ERROR;
    }
    if (swt210_rg_write()) {
        record_step(50);
        io->write32(io->context, PMC_GPU_RG_CNTRL, state->original_gpu_rg);
        record_step(51);
        io->delay_us(io->context, 10);
    }
    if (!(skip & 2u)) {
        io->write32(io->context, CAR_CLOCK_CLR, GPU_CLOCKS & ~state->original_clocks);
        io->write32(io->context, CAR_CLOCK_SET, GPU_CLOCKS & state->original_clocks);
        update32(io, CAR_PLLP_OUT5, OUT5_MASK, state->original_pllp_out5);
    }
    if (!(skip & 0x10u))
        update32(io, MC_FLUSH, MC_GPU_BIT, state->original_mc_flush);
    if (skip & 1u) {
        ok = true;
    } else if (io->release_pmic) {
        ok = io->release_pmic(io->context, state->pmic_address,
                              state->original_enable, state->original_voltage);
    } else {
        ok = write_max77621(io, state->pmic_address, state->original_voltage,
                            (state->original_enable & MAX77621_ENABLE) != 0);
    }
    io->delay_us(io->context, 5000);
    state->ready = false;
    if (!ok)
        return TG_GPU_POWER_PMIC_ERROR;
    state->owned = false;
    return TG_GPU_POWER_OK;
}


