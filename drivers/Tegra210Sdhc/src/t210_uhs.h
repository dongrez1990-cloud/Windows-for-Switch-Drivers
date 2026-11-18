/* SPDX-License-Identifier: GPL-2.0-only
 * T210 (Erista, HAC-001) UHS-I/SDR104 bring-up for the nvtsdhc miniport.
 *
 * Why this file exists: the T214 profile (t214_sdmmc.h) drives the PMIC over
 * a directly mapped I2C5 controller, so 1.8 V signalling needs no ACPI
 * resource. The same trick works on T210, but the two SoCs differ in three
 * places, all verified against this console (DDR200 P0 probe):
 *
 *   I2C5 controller   T210 0x7000C500   (T214 maps 0x7000D000)
 *   PMC legacy regs   T210 0x44 / 0xE4  (T214 0x444 / 0x4E4, i.e. +0x400)
 *   SDMMC1 pad group  PMC bit 12 in both (SD_CFG2 / NO_IOPOWER)
 *
 * Everything is off by default and driven by two registry switches, because
 * this miniport owns the boot disk:
 *
 *   Parameters\Uhs1V8     = 1  -> CMD11 style 1.8 V switch (LDO2 + PMC + pads)
 *   Parameters\UhsSdr104  = 1  -> SDR104 clock (fast clock plan) + CMD19 tuning
 *
 * UhsSdr104 implies Uhs1V8. Any failure reverts to the previous 3.3 V state
 * and records the reason in the Diag key space.
 */
#ifndef T210_UHS_H
#define T210_UHS_H

#define T210_I2C5_PHYS        0x7000C500UL
#define T210_PMC_PWR_DET_VAL  0x0E4U
#define T210_PMC_NO_IOPOWER   0x044U
#define T210_PMC_IO_BIT       (1U << 12)     /* SDMMC1 IO power domain */
#define T210_MISC_PAD_BASE    0x3000U        /* SDMMC1 CLK/CMD/DAT3..DAT0 */
#define T210_MISC_PAD_END     0x3014U
#define T210_PAD_AUTOCAL      0x1E0U         /* SD_SDMEMCMPPADCTL */

#include "../../common/tegra_i2c5_gate.h"

static VOID T210Release(PSDHC_EXTENSION s);
static NTSTATUS T210ClockSdr104(PSDHC_EXTENSION s);
static NTSTATUS T210Tune(PSDHC_EXTENSION s);

static PUCHAR T210Map(ULONG Address, ULONG Length)
{
    PHYSICAL_ADDRESS pa;
    pa.QuadPart = Address;
    return (PUCHAR)MmMapIoSpaceEx(pa, Length, PAGE_READWRITE | PAGE_NOCACHE);
}

static ULONG T210ReadReg(PUCHAR Base, ULONG Offset)
{
    return READ_REGISTER_ULONG((volatile ULONG *)(Base + Offset));
}

static VOID T210WriteReg(PUCHAR Base, ULONG Offset, ULONG Value)
{
    WRITE_REGISTER_ULONG((volatile ULONG *)(Base + Offset), Value);
}

static BOOLEAN T210WaitBits(PUCHAR Base, ULONG Offset, ULONG Mask, ULONG Value, ULONG Us)
{
    ULONG Spent;
    for (Spent = 0; Spent < Us; Spent += 5) {
        if ((T210ReadReg(Base, Offset) & Mask) == Value) return TRUE;
        KeStallExecutionProcessor(5);
    }
    return (T210ReadReg(Base, Offset) & Mask) == Value;
}

/* One MAX77620 transaction over the mapped I2C5 controller. Same packet format
   as T214PmicMessage: it is the controller's normal-mode short message path. */
static NTSTATUS T210PmicMessage(PSDHC_EXTENSION s, BOOLEAN Read, ULONG *Word, ULONG Bytes)
{
    PUCHAR b = s->T210I2c;

    T210WriteReg(b, 4, (0x3cu << 1) | (Read ? 1u : 0u));
    if (!Read) T210WriteReg(b, 12, *Word);
    T210WriteReg(b, 0, 0x2800u | (Read ? 0x40u : 0u) | ((Bytes - 1) << 1));
    T210WriteReg(b, 0x8c, 0x25);
    if (!T210WaitBits(b, 0x8c, 1, 0, 10000)) return STATUS_IO_TIMEOUT;
    T210WriteReg(b, 0, T210ReadReg(b, 0) | 0x200);
    KeStallExecutionProcessor(10);
    if (!T210WaitBits(b, 0x1c, 0x100, 0, 20000)) return STATUS_IO_TIMEOUT;
    if (T210ReadReg(b, 0x1c) & 0xf) return STATUS_IO_DEVICE_ERROR;
    if (Read) *Word = T210ReadReg(b, 12);
    return STATUS_SUCCESS;
}

/* MAX77620 LDO2 is the SD card's IO rail (L4T: vqmmc-supply = &max77620_ldo2).
   Register 0x27 holds the LDO2 configuration; the voltage selector is
   (mV - 800) / 50 in the low six bits. This console reports 0xF2 = 3.3 V. */
static NTSTATUS T210Ldo2(PSDHC_EXTENSION s, ULONG Millivolts, BOOLEAN On)
{
    NTSTATUS st = TegraI2c5Acquire(s->T210Gate);
    ULONG cfg, mask, value = 0x27, check = 0x27;

    if (!NT_SUCCESS(st)) return st;
    cfg = T210ReadReg(s->T210I2c, 0);
    mask = T210ReadReg(s->T210I2c, 0x64);
    T210WriteReg(s->T210I2c, 0x64, 0);
    if (!T210WaitBits(s->T210I2c, 0x1c, 0x100, 0, 20000)) { st = STATUS_IO_TIMEOUT; goto done; }
    st = T210PmicMessage(s, FALSE, &value, 1);
    if (NT_SUCCESS(st)) st = T210PmicMessage(s, TRUE, &value, 1);
    if (NT_SUCCESS(st)) {
        ULONG sel = (Millivolts - 800) / 50;
        value = On ? ((value & 0xc0) | (sel & 0x3f) | 0xc0) : (value & 0x3f);
        ULONG packet = 0x27 | ((value & 0xff) << 8);
        st = T210PmicMessage(s, FALSE, &packet, 2);
        if (NT_SUCCESS(st)) st = T210PmicMessage(s, FALSE, &check, 1);
        if (NT_SUCCESS(st)) st = T210PmicMessage(s, TRUE, &check, 1);
        if (NT_SUCCESS(st) && (check & 0xff) != (value & 0xff)) st = STATUS_DEVICE_POWER_FAILURE;
        if (NT_SUCCESS(st)) s->T210Ldo2Value = value & 0xff;
    }
done:
    T210WriteReg(s->T210I2c, 0, cfg & ~0x200u);
    T210WriteReg(s->T210I2c, 0x8c, 0x25);
    if (!T210WaitBits(s->T210I2c, 0x8c, 1, 0, 10000)) st = STATUS_IO_TIMEOUT;
    T210WriteReg(s->T210I2c, 0x64, mask);
    TegraI2c5Release(s->T210Gate);
    return st;
}

/* Card power enable is GPIO E4 on this board as well (L4T: vmmc = en_vdd_sd). */
static VOID T210CardPower(PSDHC_EXTENSION s, BOOLEAN On)
{
    T210WriteReg(s->T210Gpio, 0x1a0, (1u << 12) | (On ? (1u << 4) : 0));
    (VOID)T210ReadReg(s->T210Gpio, 0x120);
}

/* Bring the card back to 3.3 V: this is the only path that is known good on
   this console, so every failure funnels through it. */
static NTSTATUS T210Power3V3(PSDHC_EXTENSION s)
{
    USHORT clock = SdhcReadRegisterUshort(s, SDHC_CLOCK_CONTROL);

    SdhcWriteRegisterUshort(s, SDHC_CLOCK_CONTROL, clock & ~SDHC_CC_CLOCK_ENABLE);
    SdhcWriteRegisterUchar(s, SDHC_POWER_CONTROL, 0);
    (VOID)T210Ldo2(s, 3300, FALSE);
    SdPortWait(4000);
    T210CardPower(s, FALSE);
    SdPortWait(10000);
    T210WriteReg(s->T210Pmc, T210_PMC_NO_IOPOWER, T210ReadReg(s->T210Pmc, T210_PMC_NO_IOPOWER) | T210_PMC_IO_BIT);
    T210WriteReg(s->T210Pmc, T210_PMC_PWR_DET_VAL, T210ReadReg(s->T210Pmc, T210_PMC_PWR_DET_VAL) | T210_PMC_IO_BIT);
    T210WriteReg(s->T210Pmc, T210_PMC_NO_IOPOWER, T210ReadReg(s->T210Pmc, T210_PMC_NO_IOPOWER) & ~T210_PMC_IO_BIT);
    T210CardPower(s, TRUE);
    SdPortWait(10000);
    if (!NT_SUCCESS(T210Ldo2(s, 3300, TRUE))) return STATUS_DEVICE_POWER_FAILURE;
    SdPortWait(1000);
    s->T210Voltage18 = FALSE;
    s->T210Tuned = FALSE;
    SdhcWriteRegisterUchar(s, SDHC_POWER_CONTROL, SDHC_PC_3_3V | SDHC_PC_BUS_POWER);
    SdhcWriteRegisterUshort(s, SDHC_CLOCK_CONTROL, clock);
    return STATUS_SUCCESS;
}

/* Switch the card's signalling to 1.8 V. Ordering and waits follow the
   accepted T214 sequence; only the PMC offsets differ. */
static NTSTATUS T210Signaling(PSDHC_EXTENSION s, BOOLEAN Enable)
{
    NTSTATUS st;
    USHORT clock;
    ULONG off;
    USHORT hc;

    if (Enable == s->T210Voltage18) return STATUS_SUCCESS;
    if (!Enable) return T210Power3V3(s);
    if (!s->T210Ready) return STATUS_NOT_SUPPORTED;

    clock = SdhcReadRegisterUshort(s, SDHC_CLOCK_CONTROL);
    if (SdhcReadRegisterUlong(s, SDHC_PRESENT_STATE) & SDHC_PS_DAT_3_0) {
        s->T210VoltageStatus = STATUS_DEVICE_BUSY;
        return STATUS_DEVICE_BUSY;
    }
    SdhcWriteRegisterUshort(s, SDHC_CLOCK_CONTROL, clock & ~SDHC_CC_CLOCK_ENABLE);

    st = T210Ldo2(s, 1800, TRUE);
    if (!NT_SUCCESS(st)) goto fail;
    KeStallExecutionProcessor(200);

    T210WriteReg(s->T210Pmc, T210_PMC_PWR_DET_VAL,
                 T210ReadReg(s->T210Pmc, T210_PMC_PWR_DET_VAL) & ~T210_PMC_IO_BIT);
    for (off = T210_MISC_PAD_BASE; off <= T210_MISC_PAD_END; off += 4) {
        T210WriteReg(s->T210Misc, off, T210ReadReg(s->T210Misc, off) | (1u << 12));
    }

    /* Pad auto-calibration is optional: T210 vendor registers 0x1E0/0x1E4
       exist (P0), but B01's 50 ohm fallback write is not used here. */
    T210WriteReg(s->BaseAddress, 0x1E4, (T210ReadReg(s->BaseAddress, 0x1E4) & 0xffff8080u) | 0x606u);
    T210WriteReg(s->BaseAddress, T210_PAD_AUTOCAL, (T210ReadReg(s->BaseAddress, T210_PAD_AUTOCAL) & ~0xfu) | (1u << 31));
    KeStallExecutionProcessor(10);
    T210WriteReg(s->BaseAddress, 0x1E4, T210ReadReg(s->BaseAddress, 0x1E4) | (1u << 29) | (1u << 31));
    (VOID)T210WaitBits(s->BaseAddress, 0x1EC, 1u << 31, 0, 10000);
    T210WriteReg(s->BaseAddress, T210_PAD_AUTOCAL, T210ReadReg(s->BaseAddress, T210_PAD_AUTOCAL) & ~(1u << 31));

    hc = SdhcReadRegisterUshort(s, SDHC_HOST_CONTROL2) | SDHC_HC2_1_8V_SIGNALING;
    SdhcWriteRegisterUshort(s, SDHC_HOST_CONTROL2, hc);
    SdhcWriteRegisterUchar(s, SDHC_POWER_CONTROL, SDHC_PC_1_8V | SDHC_PC_BUS_POWER);
    SdPortWait(5000);
    SdhcWriteRegisterUshort(s, SDHC_CLOCK_CONTROL, clock | SDHC_CC_CLOCK_ENABLE);
    SdPortWait(1000);

    if ((SdhcReadRegisterUshort(s, SDHC_HOST_CONTROL2) & SDHC_HC2_1_8V_SIGNALING) &&
        (SdhcReadRegisterUlong(s, SDHC_PRESENT_STATE) & SDHC_PS_DAT_3_0) == SDHC_PS_DAT_3_0) {
        s->T210Voltage18 = TRUE;
        s->T210VoltageStatus = STATUS_SUCCESS;
        return STATUS_SUCCESS;
    }
    st = STATUS_DEVICE_PROTOCOL_ERROR;
fail:
    s->T210VoltageStatus = st;
    s->T210VoltageFailures++;
    SdhcWriteRegisterUshort(s, SDHC_CLOCK_CONTROL, clock);
    (VOID)T210Power3V3(s);
    return st;
}

/* 1.8 V state is verified by the caller through DAT[3:0] plus HC2, but a stale
   card can hold them; the ladder in T210Signaling already power-cycles. */
static NTSTATUS T210UhsInit(PSDHC_EXTENSION s)
{
    s->T210Car = T210Map(0x60006000, 0x1000);
    s->T210Misc = T210Map(0x70000000, 0x4000);
    s->T210Pmc = T210Map(0x7000E000, 0x1000);
    s->T210I2c = T210Map(T210_I2C5_PHYS, 0x1000);
    s->T210Gpio = T210Map(0x6000D000, 0x1000);
    s->T210GateHandle = NULL;
    s->T210Gate = TegraI2c5OpenGate(&s->T210GateHandle);
    if (!s->T210Car || !s->T210Misc || !s->T210Pmc || !s->T210I2c || !s->T210Gpio || !s->T210Gate) {
        T210Release(s);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    s->T210Ready = TRUE;
    return STATUS_SUCCESS;
}

static VOID T210Release(PSDHC_EXTENSION s)
{
    if (s->T210Car) { MmUnmapIoSpace(s->T210Car, 0x1000); s->T210Car = NULL; }
    if (s->T210Misc) { MmUnmapIoSpace(s->T210Misc, 0x4000); s->T210Misc = NULL; }
    if (s->T210Pmc) { MmUnmapIoSpace(s->T210Pmc, 0x1000); s->T210Pmc = NULL; }
    if (s->T210I2c) { MmUnmapIoSpace(s->T210I2c, 0x1000); s->T210I2c = NULL; }
    if (s->T210Gpio) { MmUnmapIoSpace(s->T210Gpio, 0x1000); s->T210Gpio = NULL; }
    s->T210Ready = FALSE;
}

/* ---------------------------------------------------------------- SDR104 */

/* PLLC4 OUT0 = 998.4 MHz (N=104, M=4) is already programmed on this console
   (P0: CAR 0x5a4 = 0x00046804), it is only not enabled/locked. Enable it with
   the accepted T214 sequence, then select it as the SDMMC1 source.

   CAR 0x150 (CLK_SOURCE_SDMMC1): mux is bits [31:29]. On T210 the mux value
   for PLLC4_OUT0 is NOT verified on hardware yet, so it is a registry knob:
   Parameters\UhsMux (default 3, the T214 value). Divider [7:0] = 998.4/rate-2. */
static NTSTATUS T210ClockSdr104(PSDHC_EXTENSION s)
{
    PUCHAR c = s->T210Car;
    ULONG base = T210ReadReg(c, 0x5a4);
    ULONG mux = (s->T210UhsMux > 0 && s->T210UhsMux < 8) ? s->T210UhsMux : 3;
    ULONG div = (998400u + 199680u - 1u) / 199680u;   /* 5 -> 199.68 MHz */
    ULONG source;
    ULONG i;

    s->T210PllBefore = base;
    if (!(base & (1u << 30))) {
        T210WriteReg(c, 0x5a8, T210ReadReg(c, 0x5a8) | (1u << 30));
        T210WriteReg(c, 0x5a4, base & ~((1u << 30) | (1u << 18)));
        KeStallExecutionProcessor(10);
        T210WriteReg(c, 0x5a4, (104u << 8) | 4u);
        T210WriteReg(c, 0x5a4, (1u << 30) | (104u << 8) | 4u);
    }
    for (i = 0; i < 1000; i += 5) {
        base = T210ReadReg(c, 0x5a4);
        if (base & (1u << 27)) break;
        KeStallExecutionProcessor(5);
    }
    s->T210PllAfter = base;
    if ((base & ((1u << 30) | (1u << 27))) != ((1u << 30) | (1u << 27))) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    if (div < 2 || div > 257) return STATUS_NOT_SUPPORTED;

    source = (T210ReadReg(c, 0x150) & ~0xe00000ffu) | (mux << 29) | ((div - 2) & 0xffu);
    T210WriteReg(c, 0x150, source);
    (VOID)T210ReadReg(c, 0x150);
    s->T210CarSource = source;
    s->T210BaseKhz = 199680;
    return STATUS_SUCCESS;
}

/* CMD19 execute-tuning, ported from T214Tune. The Tegra vendor tuning-step
   registers (0x30/0x34/0x38/0x1c0/0x1c4) exist on this controller (P0 dump). */
static NTSTATUS T210Tune(PSDHC_EXTENSION s)
{
    PUCHAR b = s->BaseAddress;
    ULONG signals, enables;
    USHORT clock, hc;
    NTSTATUS st = STATUS_IO_DEVICE_ERROR;
    ULONG i;

    if (!s->T210Voltage18) return STATUS_INVALID_DEVICE_STATE;
    s->T210Tuned = FALSE;
    signals = T210ReadReg(b, 0x38);
    enables = T210ReadReg(b, 0x34);
    clock = SdhcReadRegisterUshort(s, SDHC_CLOCK_CONTROL);
    T210WriteReg(b, 0x38, 0);
    T210WriteReg(b, 0x34, 0x037f8023);
    T210WriteReg(b, 0x1c4, 0);
    T210WriteReg(b, 0x1c0, (T210ReadReg(b, 0x1c0) & ~0xffc0u) | (2u << 13) | (1u << 6) | (1u << 17));
    hc = SdhcReadRegisterUshort(s, SDHC_HOST_CONTROL2);
    hc = (USHORT)((hc & ~SDHC_HC2_SELECT_SAMPLING_CLOCK) | SDHC_HC2_EXECUTE_TUNING);
    SdhcWriteRegisterUshort(s, SDHC_HOST_CONTROL2, hc);
    for (i = 0; i < 128u; ++i) {
        BOOLEAN ready = FALSE;
        ULONG tries, wait;
        s->T210TuneIterations = i + 1u;
        if (!T210WaitBits(b, SDHC_PRESENT_STATE, 3, 0, 10000)) break;
        T210WriteReg(b, 0x30, 0xffffffffu);
        SdhcWriteRegisterUshort(s, SDHC_BLOCK_SIZE, 64);
        SdhcWriteRegisterUshort(s, SDHC_BLOCK_COUNT, 1);
        SdhcWriteRegisterUshort(s, SDHC_TRANSFER_MODE, SDHC_TM_TRANSFER_READ);
        SdhcWriteRegisterUshort(s, SDHC_CLOCK_CONTROL, (USHORT)(clock & ~SDHC_CC_CLOCK_ENABLE));
        SdhcWriteRegisterUlong(s, SDHC_ARGUMENT, 0);
        SdhcWriteRegisterUshort(s, SDHC_COMMAND, (USHORT)((19u << 8) | 0x3a));
        KeStallExecutionProcessor(1);
        SdhcWriteRegisterUchar(s, SDHC_RESET, SDHC_RESET_CMD | SDHC_RESET_DAT);
        for (tries = 0; tries < 1000; tries += 5) {
            if (!(SdhcReadRegisterUchar(s, SDHC_RESET) & 6)) break;
            KeStallExecutionProcessor(5);
        }
        if (tries >= 1000) break;
        SdhcWriteRegisterUshort(s, SDHC_CLOCK_CONTROL, (USHORT)(clock | SDHC_CC_CLOCK_ENABLE));
        for (wait = 0; wait < 5000; wait += 5) {
            USHORT events = SdhcReadRegisterUshort(s, SDHC_INTERRUPT_STATUS);
            if (events & SDHC_IS_BUFFER_READ_READY) { ready = TRUE; break; }
            if (events & SDHC_IS_ERROR_INTERRUPT) break;
            KeStallExecutionProcessor(5);
        }
        SdhcWriteRegisterUshort(s, SDHC_INTERRUPT_STATUS, SDHC_IS_BUFFER_READ_READY);
        KeStallExecutionProcessor(1);
        hc = SdhcReadRegisterUshort(s, SDHC_HOST_CONTROL2);
        if (!(hc & SDHC_HC2_EXECUTE_TUNING)) {
            if (ready && (hc & SDHC_HC2_SELECT_SAMPLING_CLOCK)) st = STATUS_SUCCESS;
            break;
        }
    }
    if (!NT_SUCCESS(st)) {
        hc = (USHORT)(SdhcReadRegisterUshort(s, SDHC_HOST_CONTROL2) &
                      ~(SDHC_HC2_EXECUTE_TUNING | SDHC_HC2_SELECT_SAMPLING_CLOCK));
        SdhcWriteRegisterUshort(s, SDHC_HOST_CONTROL2, hc);
        SdhcWriteRegisterUchar(s, SDHC_RESET, SDHC_RESET_CMD | SDHC_RESET_DAT);
        for (i = 0; i < 1000 && (SdhcReadRegisterUchar(s, SDHC_RESET) & 6); i += 5) KeStallExecutionProcessor(5);
    }
    s->T210Tuned = NT_SUCCESS(st);
    s->T210TuneStatus = st;
    s->T210Tap = (T210ReadReg(b, 0x100) >> 16) & 255u;
    SdhcWriteRegisterUshort(s, SDHC_CLOCK_CONTROL, clock);
    T210WriteReg(b, 0x30, 0xffffffffu);
    T210WriteReg(b, 0x34, enables);
    T210WriteReg(b, 0x38, signals);
    return st;
}

#endif /* T210_UHS_H */
