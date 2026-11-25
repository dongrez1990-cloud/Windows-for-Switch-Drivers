/* SPDX-License-Identifier: GPL-2.0-only
 * T210 (Erista, HAC-001) UHS-I/SDR104 bring-up for the nvtsdhc miniport.
 *
 * Why this file exists: the T214 profile (t214_sdmmc.h) drives the PMIC over
 * a directly mapped I2C5 controller, so 1.8 V signalling needs no ACPI
 * resource. The same trick works on T210, but the two SoCs differ in three
 * places, all verified against this console (DDR200 P0 probe):
 *
 *   I2C5 controller   T210 0x7000D000   (T214 maps 0x7000D000)
 *   PMC legacy regs   T210 0x444 / 0x4E4 at PMC base 0x7000E400
 *                     (0x7000E000 is RTC, not PMC - the first build wrote
 *                      there, so the 1.8 V IO-domain sequence did nothing)
 *   SDMMC1 pad group  PMC bit 12 in both (SD_CFG2 / NO_IOPOWER)
 *
 * The first T210 build used 0x7000C500 here, which on Tegra210 is I2C3, not
 * I2C5; its PMIC read failed (UhsStLdo2 = 0xFFFFFFFF). Both bases are probed
 * now and the one that answers is recorded, so the fix is data-driven.
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

#define T210_I2C5_PHYS        0x7000D000UL
#define T210_I2C3_PHYS        0x7000C500UL   /* what the first build wrongly used */
#define T210_PMC_PHYS        0x7000E400UL   /* PMC; 0x7000E000 is RTC */
#define T210_PMC_PWR_DET_VAL  0x0E4U
#define T210_PMC_NO_IOPOWER   0x044U
#define T210_PMC_IO_BIT       (1U << 12)     /* SDMMC1 IO power domain */
#define T210_MISC_PAD_BASE    0x3000U        /* SDMMC1 CLK/CMD/DAT3..DAT0 */
#define T210_MISC_PAD_END     0x3014U
/* PINMUX_AUX pull field [3:2]: 1 = pull-down, 2 = pull-up, 0 = none.
   1.8 V signalling needs no pulls (Linux tegra 1v8 pad table); 3.3 V uses
   pull-down on CLK and pull-up on CMD/DAT, exactly as UEFI's SdPowerOn(). */
#define T210_PAD_PULL_MASK    0x0000000cU
#define T210_PAD_BIT12        0x00001000U
#define T210_PAD_AUTOCAL      0x1E0U         /* SD_SDMEMCMPPADCTL */

#include "../../common/tegra_i2c5_gate.h"

/* Some build profiles (NV_T210_UHS_NO_CAR: 1.8 V only, CAR untouched) never
   call the clock/tuning helpers below; that is intentional, not dead code. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"

static VOID T210Release(PSDHC_EXTENSION s);
static NTSTATUS T210ClockSdr104(PSDHC_EXTENSION s);
static NTSTATUS T210Tune(PSDHC_EXTENSION s);
#if NV_T210_UHS_POSTCHECK || NV_T210_UHS_POSTCHECK2
static VOID T210PostCheck(PSDHC_EXTENSION s);
#endif
#if NV_T210_UHS_DMA
static VOID T210DmaInit(PSDHC_EXTENSION s);
#endif

/* Deferred-worker profile: the card is the running boot disk, so a failed
   switch must not power-cycle it. Soft mode restores rails/registers only. */
static BOOLEAN T210SoftMode;

/* NV_T210_UHS_SOFT_FAIL=1 keeps the same rule for the boot-time (SDPORT)
   profile: a failed 1.8 V switch restores 3.3 V and reports the error, but
   never cuts the card's supply. */
#ifndef NV_T210_UHS_SOFT_FAIL
#define NV_T210_UHS_SOFT_FAIL 0
#endif

/* NV_T210_UHS_PROBE_STOP=1: the boot-path profile has no way to leave a
   message behind (a failed switch there is INACCESSIBLE_BOOT_DEVICE, and no
   dump, log or registry key survives). So every failure point raises a
   bugcheck whose code names the step; the blue screen shows it as hex.
   Only for bring-up builds of the risky profile. */
#ifndef NV_T210_UHS_PROBE_STOP
#define NV_T210_UHS_PROBE_STOP 0
#endif

#define T210_STOP_INIT     0xDA01u   /* register mapping or I2C5 gate failed */
#define T210_STOP_NOTREADY 0xDA02u   /* signalling asked for before init     */
#define T210_STOP_DATBUSY  0xDA03u   /* DAT[3:0] not in the CMD11 low state  */
#define T210_STOP_LDO2     0xDA04u   /* LDO2 could not be set to 1.8 V       */
#define T210_STOP_VERIFY   0xDA05u   /* HC2/DAT verification after switch    */
#define T210_STOP_BACK3V3  0xDA06u   /* restore to 3.3 V failed              */
#define T210_STOP_SWITCHED 0xDA07u   /* deliberate: switch ran and verified  */

/* NV_T210_UHS_STOP_AFTER_SWITCH=1 stops the machine right after a verified
   1.8 V switch, before SDPORT continues. Seeing 0xDA07 proves SDPORT reaches
   SdhcSetSignaling and that the platform sequence itself completed; a plain
   0x7B instead means the failure is upstream of our callback. */
#ifndef NV_T210_UHS_STOP_AFTER_SWITCH
#define NV_T210_UHS_STOP_AFTER_SWITCH 0
#endif

/* NV_T210_UHS_POSTCHECK=1: right after a verified switch, ask the card one
   CMD6 query through the raw path that stage1 proved works, and report the
   answer as a bugcheck code. This separates "the registers say 1.8 V" from
   "the card still talks at 1.8 V", which the boot failure could not. */
#ifndef NV_T210_UHS_POSTCHECK
#define NV_T210_UHS_POSTCHECK 0
#endif
/* Post-check after SDPORT has programmed the SDR104 timing instead of right
   after the switch: the card may legitimately not answer while the host is
   still on the old (HS) timing. */
#ifndef NV_T210_UHS_POSTCHECK2
#define NV_T210_UHS_POSTCHECK2 0
#endif
#define T210_STOP_POST_OK   0xDA08u   /* card answered CMD6 at 1.8 V          */
#define T210_STOP_POST_FAIL 0xDA09u   /* card did not answer CMD6 at 1.8 V    */
#define T210_STOP_TUNE_OK   0xDA0Bu   /* vendor tuning completed              */
#define T210_STOP_TUNE_FAIL 0xDA0Cu   /* vendor tuning failed                 */

#ifndef NV_T210_UHS_POSTCHECK3
#define NV_T210_UHS_POSTCHECK3 0
#endif

/* NV_T210_UHS_BEACON=1: a single rich beacon. It stops the machine at a point
   of interest and packs the state into the bugcheck parameters, so one boot
   reports several facts at once (the blue screen prints all four). */
#ifndef NV_T210_UHS_BEACON
#define NV_T210_UHS_BEACON 0
#endif
#define T210_BEACON_TUNE 0xDA40u   /* reached SdhcExecuteTuning (T210 path) */

/* NV_T210_UHS_DMA=1 turns each 512-byte-block PIO request into a hardware
   ADMA32 transfer through a private non-cached low-memory bounce buffer.
 *
 * Why a bounce buffer and not SDPORT's scatter/gather list: this controller
 * reports _CCA = 0 (non-coherent) and the inbox stack never did the cache
 * maintenance for it, which is why the driver was pinned to PIO. Copying
 * through MmNonCached memory keeps DMA away from cached OS/user pages, so
 * there is nothing to flush or invalidate; the price is one memcpy per
 * transfer, which is cheap next to a card that moves a few MB/s.
 *
 * If anything is off (no buffer, oversized request, earlier fault) the request
 * stays on the plain PIO path - correctness first. */
#ifndef NV_T210_UHS_DMA
#define NV_T210_UHS_DMA 0
#endif

/* Data commands that must have completed before the ADMA probe runs, so the
   probe never competes with the boot volume (0 = probe at the first one). */
#ifndef NV_T210_DMA_PROBE_AFTER
#define NV_T210_DMA_PROBE_AFTER 20000
#endif

/* Seconds after the slot came up before the probe may run: boot I/O must never
   share the controller with a two-sector experiment. */
#ifndef NV_T210_DMA_PROBE_DELAY_SEC
#define NV_T210_DMA_PROBE_DELAY_SEC 300
#endif

/* Seconds of disk idleness required right before the probe (0 = none). */
#ifndef NV_T210_DMA_PROBE_IDLE_SEC
#define NV_T210_DMA_PROBE_IDLE_SEC 0
#endif

/* The probe fires on the first read command of at least this many blocks after
   the boot gates above are satisfied. NTFS read-ahead rewrites small requests
   into 32-block commands, so an exact size is not a usable trigger. */
#ifndef NV_T210_DMA_PROBE_MIN_BLOCKS
#define NV_T210_DMA_PROBE_MIN_BLOCKS 8
#endif

/* 1 = take the ADMA-vs-PIO verdict inside a request DPC (bus known idle);
   0 = keep the old timer thread that waits for a quiet bus. */
#ifndef NV_T210_DMA_PROBE_IN_DPC
#define NV_T210_DMA_PROBE_IN_DPC 1
#endif

/* Bring-up safety: accelerate reads first. Writes keep the PIO path until the
   DMA path has proven itself (a bad write would damage the boot volume, a bad
   read only costs a retry). */
#ifndef NV_T210_UHS_DMA_WRITES
#define NV_T210_UHS_DMA_WRITES 0
#endif

#define T210_DMA_BYTES        (512u * 1024u)   /* data area per request */
#define T210_DMA_TABLE_BYTES  4096u            /* ADMA2 table after it  */

/* NV_T210_UHS_NO_AUTOCAL=1 skips the vendor pad auto-calibration inside the
   1.8 V switch. Two reasons to try it: the auto-cal wait (10 ms) plus its
   extra register traffic delays the 1.8 V clock burst the card expects right
   after its CMD11 response, and its PU/PD offset value (0x606) is unverified
   at 1.8 V on this console. */
#ifndef NV_T210_UHS_NO_AUTOCAL
#define NV_T210_UHS_NO_AUTOCAL 0
#endif

/* NV_T210_UHS_FAKE_33V=1 is a control experiment: run the whole 1.8 V
   sequence but keep LDO2 at 3.3 V. If the card answers the post-check CMD6
   with this build, the sequence itself is fine and the fault is the rail
   voltage; if it does not, the sequence (or a step in it) breaks the bus. */
#ifndef NV_T210_UHS_FAKE_33V
#define NV_T210_UHS_FAKE_33V 0
#endif

/* NV_T210_UHS_KEEP_PULLS=1 leaves the pad pull-ups alone during the 1.8 V
   switch. Clearing them (the Linux 1v8 pad table) makes the DAT lines float,
   and both our verification and SDPORT's own post-switch check can then read
   the wrong level - which is one candidate for the observed "voltage switched,
   then reverted by the port driver". */
#ifndef NV_T210_UHS_KEEP_PULLS
#define NV_T210_UHS_KEEP_PULLS 0
#endif

#if NV_T210_UHS_PROBE_STOP
#define T210Stop(Code) KeBugCheckEx((ULONG)(Code), 0x54433231u, 0, 0, 0)
#else
#define T210Stop(Code) ((void)0)
#endif

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

/* One MAX77620 transaction over a mapped I2C controller. Same packet format
   as T214PmicMessage: it is the controller's normal-mode short message path. */
static NTSTATUS T210PmicMessageAt(PUCHAR b, BOOLEAN Read, ULONG *Word, ULONG Bytes)
{
    if (b == NULL) return STATUS_NOT_SUPPORTED;
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

static NTSTATUS T210PmicMessage(PSDHC_EXTENSION s, BOOLEAN Read, ULONG *Word, ULONG Bytes)
{
    return T210PmicMessageAt(s->T210I2c, Read, Word, Bytes);
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

/* Pad pinmux for a signalling voltage: 3.3 V keeps UEFI's pulls (CLK
   pull-down, CMD/DAT pull-up), 1.8 V has none (Linux tegra 1v8 pad table).
   [1:0] keep the pin's SFIO function, bit 6 input enable, bit 13 drive 2X. */
static VOID T210PadPulls(PSDHC_EXTENSION s, BOOLEAN Voltage18)
{
    ULONG off;

    for (off = T210_MISC_PAD_BASE; off <= T210_MISC_PAD_END; off += 4) {
        ULONG v = T210ReadReg(s->T210Misc, off) & ~(T210_PAD_PULL_MASK | T210_PAD_BIT12);
        if (!Voltage18) {
            /* SDMMC1_CLK_CFG is 0x3000, CMD 0x3004, DAT3..DAT0 follow. */
            v |= (off == T210_MISC_PAD_BASE) ? 0x4u /* pull-down */ : 0x8u /* pull-up */;
        }
        T210WriteReg(s->T210Misc, off, v);
    }
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
    if (!T210SoftMode) {
        T210CardPower(s, FALSE);
        SdPortWait(10000);
    }
    T210WriteReg(s->T210Pmc, T210_PMC_NO_IOPOWER, T210ReadReg(s->T210Pmc, T210_PMC_NO_IOPOWER) | T210_PMC_IO_BIT);
    T210WriteReg(s->T210Pmc, T210_PMC_PWR_DET_VAL, T210ReadReg(s->T210Pmc, T210_PMC_PWR_DET_VAL) | T210_PMC_IO_BIT);
    T210WriteReg(s->T210Pmc, T210_PMC_NO_IOPOWER, T210ReadReg(s->T210Pmc, T210_PMC_NO_IOPOWER) & ~T210_PMC_IO_BIT);
    if (!T210SoftMode) {
        T210CardPower(s, TRUE);
        SdPortWait(10000);
    }
    if (!NT_SUCCESS(T210Ldo2(s, 3300, TRUE))) {
        T210Stop(T210_STOP_BACK3V3);
        return STATUS_DEVICE_POWER_FAILURE;
    }
    SdPortWait(1000);
    T210PadPulls(s, FALSE);
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
    USHORT hc;

    if (Enable == s->T210Voltage18) return STATUS_SUCCESS;

    if (!Enable) return T210Power3V3(s);
    if (!s->T210Ready) {
        T210Stop(T210_STOP_NOTREADY);
        return STATUS_NOT_SUPPORTED;
    }

    clock = SdhcReadRegisterUshort(s, SDHC_CLOCK_CONTROL);
    if (SdhcReadRegisterUlong(s, SDHC_PRESENT_STATE) & SDHC_PS_DAT_3_0) {
        s->T210VoltageStatus = STATUS_DEVICE_BUSY;
        T210Stop(T210_STOP_DATBUSY);
        return STATUS_DEVICE_BUSY;
    }
    SdhcWriteRegisterUshort(s, SDHC_CLOCK_CONTROL, clock & ~SDHC_CC_CLOCK_ENABLE);

    st = T210Ldo2(s, NV_T210_UHS_FAKE_33V ? 3300u : 1800u, TRUE);
    if (!NT_SUCCESS(st)) {
        T210Stop(T210_STOP_LDO2);
        goto fail;
    }
    KeStallExecutionProcessor(200);

    T210WriteReg(s->T210Pmc, T210_PMC_PWR_DET_VAL,
                 T210ReadReg(s->T210Pmc, T210_PMC_PWR_DET_VAL) & ~T210_PMC_IO_BIT);
    /* No pulls at 1.8 V; keeping UEFI's 3.3 V pull-ups here is what makes the
       1.8 V handshake unusable. */
#if !NV_T210_UHS_KEEP_PULLS
    T210PadPulls(s, TRUE);
#endif

#if !NV_T210_UHS_NO_AUTOCAL
    /* Pad auto-calibration is optional: T210 vendor registers 0x1E0/0x1E4
       exist (P0), but B01's 50 ohm fallback write is not used here. */
    T210WriteReg(s->BaseAddress, 0x1E4, (T210ReadReg(s->BaseAddress, 0x1E4) & 0xffff8080u) | 0x606u);
    T210WriteReg(s->BaseAddress, T210_PAD_AUTOCAL, (T210ReadReg(s->BaseAddress, T210_PAD_AUTOCAL) & ~0xfu) | (1u << 31));
    KeStallExecutionProcessor(10);
    T210WriteReg(s->BaseAddress, 0x1E4, T210ReadReg(s->BaseAddress, 0x1E4) | (1u << 29) | (1u << 31));
    (VOID)T210WaitBits(s->BaseAddress, 0x1EC, 1u << 31, 0, 10000);
    T210WriteReg(s->BaseAddress, T210_PAD_AUTOCAL, T210ReadReg(s->BaseAddress, T210_PAD_AUTOCAL) & ~(1u << 31));
#endif

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
#if NV_T210_UHS_STOP_AFTER_SWITCH
        T210Stop(T210_STOP_SWITCHED);
#endif
#if NV_T210_UHS_POSTCHECK
        T210PostCheck(s);
#endif
        return STATUS_SUCCESS;
    }
    st = STATUS_DEVICE_PROTOCOL_ERROR;
    T210Stop(T210_STOP_VERIFY);
fail:
    s->T210VoltageStatus = st;
    s->T210VoltageFailures++;
    SdhcWriteRegisterUshort(s, SDHC_CLOCK_CONTROL, clock);
    (VOID)T210Power3V3(s);
    return st;
}

/* 1.8 V state is verified by the caller through DAT[3:0] plus HC2, but a stale
   card can hold them; the ladder in T210Signaling already power-cycles. */
#if NV_T210_UHS_DMA
/* ---------------------------------------------------------- bounce-buffer DMA */

/* One contiguous non-cached low-memory block: data area, then the ADMA2
   descriptor table. Non-cached means the device and the CPU see the same
   bytes with no flush/invalidate anywhere. */
static VOID T210DmaInit(PSDHC_EXTENSION s)
{
    PHYSICAL_ADDRESS Low, High, Boundary;
    SIZE_T Total = T210_DMA_BYTES + T210_DMA_TABLE_BYTES;

    if (s->T210Dma != NULL) return;
    Low.QuadPart = 0x100000;                 /* below 4 GB for ADMA32 */
    High.QuadPart = 0xFFFF0000;
    Boundary.QuadPart = 0;
    s->T210Dma = (PUCHAR)MmAllocateContiguousMemorySpecifyCache(Total, Low, High,
                                                                Boundary, MmNonCached);
    if (s->T210Dma == NULL) {
        s->T210DmaCapacity = 0;
        return;
    }
    RtlZeroMemory(s->T210Dma, Total);
    s->T210DmaPa = MmGetPhysicalAddress(s->T210Dma);
    {
        /* MC block: SMMU translation enable words. T214 only uses physical DMA
           when the SDMMC1A client's translation is off; record them here so the
           diag shows whether that is the case on this console. */
        PHYSICAL_ADDRESS mcPa;
        PULONG mc;
        mcPa.QuadPart = 0x70019000;
        mc = (PULONG)MmMapIoSpaceEx(mcPa, PAGE_SIZE, PAGE_READONLY | PAGE_NOCACHE);
        if (mc != NULL) {
            s->T210Smmu = mc[0x0a88/4];
            s->T210Smmu2 = (mc[0x0a90/4] << 16) ^ mc[0x0a94/4];
            MmUnmapIoSpace(mc, PAGE_SIZE);
        }
    }
    s->T210DmaCapacity = T210_DMA_BYTES;
    s->T210DmaFault = FALSE;
}

/* Turn the current 512-byte-block PIO request into an ADMA32 transfer.
   Returns TRUE when the request was taken over; FALSE leaves plain PIO. */
static BOOLEAN T210BuildDma(PSDHC_EXTENSION s, PSDPORT_REQUEST r)
{
    PSDPORT_COMMAND c = &r->Command;
    PULONG Table;
    ULONG Size, Done, Index;
    UCHAR HostControl;

    s->T210DmaActive = FALSE;
    s->T210DmaComplete = FALSE;
    if (c->Index != 17 && c->Index != 18 && c->Index != 24 && c->Index != 25) return FALSE;

    Size = c->BlockCount * 512u;
    if (!s->T210DmaOk || s->T210DmaFault || s->T210Dma == NULL || c->DataBuffer == NULL ||
        c->TransferMethod != SdTransferMethodPio || c->BlockSize != 512 ||
        c->BlockCount == 0 || Size > s->T210DmaCapacity || c->Length < Size ||
        (!NV_T210_UHS_DMA_WRITES &&
         c->TransferDirection == SdTransferDirectionWrite)) {
        s->T210PioFallbacks++;
        return FALSE;
    }

    s->T210DmaRequests++;
    s->T210DmaMaxBlocks = MAX(s->T210DmaMaxBlocks, c->BlockCount);

    /* Writes go through the bounce buffer; reads come back through it. */
    if (c->TransferDirection == SdTransferDirectionWrite) {
        RtlCopyMemory(s->T210Dma, c->DataBuffer, Size);
    }

    Table = (PULONG)(s->T210Dma + s->T210DmaCapacity);
    for (Done = 0, Index = 0; Done < Size; Index += 2) {
        ULONG Chunk = MIN(Size - Done, 32768u);          /* ADMA2 max per entry */
        Table[Index] = (Chunk << 16) | 0x21u |           /* valid | TRAN | 32-bit addr slot */
                       (((Done + Chunk) == Size) ? 0x2u : 0u);  /* END on last */
        Table[Index + 1] = s->T210DmaPa.LowPart + Done;
        Done += Chunk;
    }
    KeMemoryBarrier();

    HostControl = SdhcReadRegisterUchar(s, SDHC_HOST_CONTROL);
    SdhcWriteRegisterUchar(s, SDHC_HOST_CONTROL,
                           (UCHAR)((HostControl & ~SDHC_HC_DMA_SELECT_MASK) |
                                   SDHC_HC_DMA_SELECT_ADMA32));
    SdhcWriteRegisterUlong(s, SDHC_ADMA_SYSADDR_LOW,
                           s->T210DmaPa.LowPart + s->T210DmaCapacity);
    SdhcWriteRegisterUshort(s, SDHC_TRANSFER_MODE,
                            (USHORT)(SdhcReadRegisterUshort(s, SDHC_TRANSFER_MODE) |
                                     SDHC_TM_DMA_ENABLE));

    s->T210DmaTarget = c->DataBuffer;
    s->T210DmaLength = Size;
    s->T210DmaActive = TRUE;
    return TRUE;
}

/* ---------------------------------------------------------- DMA self-test */

/* One raw CMD17 (READ_SINGLE_BLOCK) for one 512-byte sector, either through
   ADMA32 into the bounce buffer, or through the buffer data port. Used only by
   the self-test below: it proves the physical-address DMA path really works
   before SDPORT's I/O is allowed to use it. */
static NTSTATUS
T210RawReadSector(
    _In_ PSDHC_EXTENSION s,
    _In_ ULONG Lba,
    _Out_writes_bytes_(512) PUCHAR Buffer,
    _In_ BOOLEAN UseDma
    )
{
    USHORT savedTm, savedBs, savedBc, savedTo, savedIe, savedEe, savedIse, savedEse;
    UCHAR savedHc;
    USHORT events = 0;
    ULONG i;
    NTSTATUS st = STATUS_IO_TIMEOUT;
    PULONG table;

    if (SdhcReadRegisterUlong(s, SDHC_PRESENT_STATE) &
        (SDHC_PS_CMD_INHIBIT | SDHC_PS_DAT_INHIBIT)) {
        return STATUS_DEVICE_BUSY;
    }

    savedHc = SdhcReadRegisterUchar(s, SDHC_HOST_CONTROL);
    savedTm = SdhcReadRegisterUshort(s, SDHC_TRANSFER_MODE);
    savedBs = SdhcReadRegisterUshort(s, SDHC_BLOCK_SIZE);
    savedBc = SdhcReadRegisterUshort(s, SDHC_BLOCK_COUNT);
    savedTo = SdhcReadRegisterUshort(s, SDHC_TIMEOUT_CONTROL);
    savedIe = SdhcReadRegisterUshort(s, SDHC_INTERRUPT_STATUS_ENABLE);
    savedEe = SdhcReadRegisterUshort(s, SDHC_ERROR_STATUS_ENABLE);
    savedIse = SdhcReadRegisterUshort(s, SDHC_INTERRUPT_SIGNAL_ENABLE);
    savedEse = SdhcReadRegisterUshort(s, SDHC_ERROR_SIGNAL_ENABLE);

    SdhcWriteRegisterUshort(s, SDHC_INTERRUPT_SIGNAL_ENABLE, 0);
    SdhcWriteRegisterUshort(s, SDHC_ERROR_SIGNAL_ENABLE, 0);
    SdhcWriteRegisterUshort(s, SDHC_INTERRUPT_STATUS, 0xffff);
    SdhcWriteRegisterUshort(s, SDHC_ERROR_STATUS, 0xffff);
    SdhcWriteRegisterUshort(s, SDHC_TIMEOUT_CONTROL, SDHC_TC_MAX_DATA_TIMEOUT);
    SdhcWriteRegisterUshort(s, SDHC_BLOCK_SIZE, 512);
    SdhcWriteRegisterUshort(s, SDHC_BLOCK_COUNT, 1);

    if (UseDma) {
        RtlZeroMemory(Buffer, 512);
        table = (PULONG)(s->T210Dma + s->T210DmaCapacity);
        table[0] = (512u << 16) | 0x21u | 0x2u;         /* 512 B, TRAN|VALID|END */
        table[1] = s->T210DmaPa.LowPart;
        KeMemoryBarrier();
        SdhcWriteRegisterUchar(s, SDHC_HOST_CONTROL,
                               (UCHAR)((savedHc & ~SDHC_HC_DMA_SELECT_MASK) |
                                       SDHC_HC_DMA_SELECT_ADMA32));
        SdhcWriteRegisterUlong(s, SDHC_ADMA_SYSADDR_LOW,
                               s->T210DmaPa.LowPart + s->T210DmaCapacity);
        SdhcWriteRegisterUshort(s, SDHC_TRANSFER_MODE,
                                SDHC_TM_TRANSFER_READ | SDHC_TM_DMA_ENABLE);
    } else {
        SdhcWriteRegisterUchar(s, SDHC_HOST_CONTROL,
                               (UCHAR)(savedHc & ~SDHC_HC_DMA_SELECT_MASK));
        SdhcWriteRegisterUshort(s, SDHC_TRANSFER_MODE, SDHC_TM_TRANSFER_READ);
    }

    SdhcWriteRegisterUlong(s, SDHC_ARGUMENT, Lba);
    SdhcWriteRegisterUshort(s, SDHC_COMMAND,
                            (USHORT)((17u << 8) | SDHC_CMD_RESPONSE_48BIT_NOBUSY |
                                     SDHC_CMD_CRC_CHECK_ENABLE | SDHC_CMD_INDEX_CHECK_ENABLE |
                                     SDHC_CMD_DATA_PRESENT));

    for (i = 0; i < 4000; ++i) {                        /* up to 200 ms */
        events = SdhcReadRegisterUshort(s, SDHC_INTERRUPT_STATUS);
        if (UseDma) {
            if (events & (SDHC_IS_TRANSFER_COMPLETE | SDHC_IS_ERROR_INTERRUPT)) break;
        } else {
            if (events & (SDHC_IS_BUFFER_READ_READY | SDHC_IS_ERROR_INTERRUPT |
                          SDHC_IS_TRANSFER_COMPLETE)) break;
        }
        KeStallExecutionProcessor(50);
    }

    if (events & SDHC_IS_ERROR_INTERRUPT) {
        st = STATUS_IO_DEVICE_ERROR;
    } else if (UseDma) {
        st = (events & SDHC_IS_TRANSFER_COMPLETE) ? STATUS_SUCCESS : STATUS_IO_TIMEOUT;
        if (NT_SUCCESS(st)) {
            KeMemoryBarrier();
            RtlCopyMemory(Buffer, s->T210Dma, 512);
        }
    } else if (events & SDHC_IS_BUFFER_READ_READY) {
        SdhcReadDataPort(s, Buffer, 512);
        for (i = 0; i < 4000 && !(events & SDHC_IS_TRANSFER_COMPLETE); ++i) {
            events = SdhcReadRegisterUshort(s, SDHC_INTERRUPT_STATUS);
            if (events & SDHC_IS_ERROR_INTERRUPT) break;
            KeStallExecutionProcessor(50);
        }
        st = (events & SDHC_IS_TRANSFER_COMPLETE) ? STATUS_SUCCESS : STATUS_IO_TIMEOUT;
    }

    /* Snapshot the controller's view of the failed/successful attempt. */
    s->T210DmaTestIs = SdhcReadRegisterUshort(s, SDHC_INTERRUPT_STATUS);
    s->T210DmaTestErr = SdhcReadRegisterUshort(s, SDHC_ERROR_STATUS);
    s->T210DmaTestAdma = SdhcReadRegisterUchar(s, SDHC_ADMA_ERROR_STATUS);
    s->T210DmaTestPs = (USHORT)SdhcReadRegisterUlong(s, SDHC_PRESENT_STATE);
    SdhcWriteRegisterUshort(s, SDHC_INTERRUPT_STATUS, 0xffff);
    SdhcWriteRegisterUshort(s, SDHC_ERROR_STATUS, 0xffff);
    SdhcWriteRegisterUchar(s, SDHC_HOST_CONTROL, savedHc);
    SdhcWriteRegisterUshort(s, SDHC_TRANSFER_MODE, savedTm);
    SdhcWriteRegisterUshort(s, SDHC_BLOCK_SIZE, savedBs);
    SdhcWriteRegisterUshort(s, SDHC_BLOCK_COUNT, savedBc);
    SdhcWriteRegisterUshort(s, SDHC_TIMEOUT_CONTROL, savedTo);
    SdhcWriteRegisterUshort(s, SDHC_INTERRUPT_STATUS_ENABLE, savedIe);
    SdhcWriteRegisterUshort(s, SDHC_ERROR_STATUS_ENABLE, savedEe);
    SdhcWriteRegisterUshort(s, SDHC_INTERRUPT_SIGNAL_ENABLE, savedIse);
    SdhcWriteRegisterUshort(s, SDHC_ERROR_SIGNAL_ENABLE, savedEse);
    return st;
}

/* Compare one sector read through ADMA against the same sector read through
   PIO. Only an exact match enables DMA for real I/O; anything else keeps the
   slot on the proven PIO path for the rest of the boot. */static VOID
T210DmaSelfTest(
    _In_ PSDHC_EXTENSION s
    )
{
    PUCHAR pio = s->T210Dma + (T210_DMA_BYTES / 2);
    NTSTATUS dmaSt, pioSt;
    ULONG mismatches = 0;
    ULONG i;

    if (s->T210DmaTested || s->T210Dma == NULL) return;
    s->T210DmaTested = TRUE;
    if (s->T210Rca == 0) { s->T210DmaTestResult = 0xE001; return; }

    /* LBA 0 (the partition table) is never written by this test. The boot disk
       keeps SDPORT busy, so a collision shows up as a failed read; retry a few
       times before deciding. */
    {
        ULONG attempt;
        dmaSt = STATUS_DEVICE_BUSY;
        pioSt = STATUS_DEVICE_BUSY;
        for (attempt = 0; attempt < 6; ++attempt) {
            LARGE_INTEGER gap;
            ULONG quiet;
            for (quiet = 0; quiet < 4; ++quiet) {           /* ~2 ms quiet */
                LARGE_INTEGER t;
                t.QuadPart = -2000LL;                      /* 200 us */
                KeDelayExecutionThread(KernelMode, FALSE, &t);
                if (s->OutstandingRequests[0] != NULL ||
                    (SdhcReadRegisterUlong(s, SDHC_PRESENT_STATE) &
                     (SDHC_PS_CMD_INHIBIT | SDHC_PS_DAT_INHIBIT)) != 0) {
                    break;
                }
            }
            if (quiet < 4) { gap.QuadPart = -50000000LL; KeDelayExecutionThread(KernelMode, FALSE, &gap); continue; }
            dmaSt = T210RawReadSector(s, 0, s->T210Dma, TRUE);
            pioSt = T210RawReadSector(s, 0, pio, FALSE);
            if (NT_SUCCESS(dmaSt) && NT_SUCCESS(pioSt)) break;
            gap.QuadPart = -50000000LL;                    /* 5 s */
            KeDelayExecutionThread(KernelMode, FALSE, &gap);
        }
        s->T210DmaTestStDma = (ULONG)dmaSt;
        s->T210DmaTestStPio = (ULONG)pioSt;
    }
    if (!NT_SUCCESS(dmaSt)) {
        s->T210DmaTestResult = 0xE002;                     /* ADMA read failed  */
        return;
    }
    if (!NT_SUCCESS(pioSt)) {
        s->T210DmaTestResult = 0xE005;                     /* PIO read failed   */
        return;
    }
    for (i = 0; i < 512; ++i) {
        if (s->T210Dma[i] != pio[i]) mismatches++;
    }
    if (mismatches != 0) {
        s->T210DmaTestResult = 0xE003u | (mismatches << 16);
        return;
    }
    s->T210DmaTestResult = 1;
    s->T210DmaOk = TRUE;
}

/* The same verdict, but taken from a place where the bus is known to be free:
   SdhcRequestDpc calls this right after a data command completed and before
   the request is handed back, so the controller is idle and no window has to
   be waited for (the old timer path almost never found one on the boot disk).
   DISPATCH_LEVEL only: no KeDelayExecutionThread here. If the controller is
   not idle the probe simply runs again on the next completion. */
static VOID
T210DmaProbe(
    _In_ PSDHC_EXTENSION s
    )
{
    PUCHAR pio;
    NTSTATUS dmaSt, pioSt;
    ULONG mismatches = 0;
    ULONG sig = 0;
    ULONG i;

    if (s->T210DmaTested || s->T210Dma == NULL || s->T210Rca == 0) return;
    if (SdhcReadRegisterUlong(s, SDHC_PRESENT_STATE) &
        (SDHC_PS_CMD_INHIBIT | SDHC_PS_DAT_INHIBIT)) {
        s->T210DmaProbeSkip++;
        return;                                        /* busy: next completion */
    }

    s->T210DmaTested = TRUE;
    pio = s->T210Dma + (T210_DMA_BYTES / 2);

    dmaSt = T210RawReadSector(s, 0, s->T210Dma, TRUE);
    s->T210DmaTestStDma = (ULONG)dmaSt;
    if (!NT_SUCCESS(dmaSt)) {
        s->T210DmaTestResult = 0xE002;                 /* ADMA read failed  */
        return;
    }
    pioSt = T210RawReadSector(s, 0, pio, FALSE);
    s->T210DmaTestStPio = (ULONG)pioSt;
    if (!NT_SUCCESS(pioSt)) {
        s->T210DmaTestResult = 0xE005;                 /* PIO read failed   */
        return;
    }

    for (i = 0; i < 512; ++i) {
        if (s->T210Dma[i] != pio[i]) mismatches++;
    }
    if (mismatches != 0) {
        s->T210DmaTestResult = 0xE003u | (mismatches << 16);
        return;
    }

    /* Identical to the PIO read, which already rules out a bad translation.
       The signature is a second, independent check that LBA 0 is real data. */
    RtlCopyMemory(&sig, s->T210Dma + 3, sizeof(sig));
    s->T210DmaSig = sig;
    if (((s->T210Dma[510] != 0x55u) || (s->T210Dma[511] != 0xAAu)) &&
        (sig != 0x5346544eu)) {                        /* 'N','T','F','S' */
        s->T210DmaTestResult = 0xE006;
        return;
    }

    s->T210DmaTestResult = 1;
    s->T210DmaOk = TRUE;
}
/* Runs from the DPC when the transfer reached TRANSFER_COMPLETE. */
static VOID T210DmaDone(PSDHC_EXTENSION s, PSDPORT_REQUEST r)
{
    KeMemoryBarrier();
    if (r->Command.TransferDirection == SdTransferDirectionRead) {
        RtlCopyMemory(s->T210DmaTarget, s->T210Dma, s->T210DmaLength);
        NvDiag.BlocksRead += s->T210DmaLength / 512;
    } else {
        NvDiag.BlocksWritten += s->T210DmaLength / 512;
    }
    s->T210DmaBlocks += s->T210DmaLength / 512;
    s->T210DmaComplete = TRUE;
    s->T210DmaActive = FALSE;
}
#endif /* NV_T210_UHS_DMA */

static NTSTATUS T210UhsInit(PSDHC_EXTENSION s)
{
    s->T210Car = T210Map(0x60006000, 0x1000);
    s->T210Misc = T210Map(0x70000000, 0x4000);
    s->T210Pmc = T210Map(T210_PMC_PHYS, 0x1000);
    s->T210I2c = T210Map(T210_I2C5_PHYS, 0x1000);
    s->T210I2cAlt = T210Map(T210_I2C3_PHYS, 0x1000);
    s->T210Gpio = T210Map(0x6000D000, 0x1000);
    s->T210GateHandle = NULL;
    s->T210Gate = TegraI2c5OpenGate(&s->T210GateHandle);
    if (!s->T210Car || !s->T210Misc || !s->T210Pmc || !s->T210I2c || !s->T210Gpio || !s->T210Gate) {
        T210Release(s);
        T210Stop(T210_STOP_INIT);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
#if NV_T210_UHS_SOFT_FAIL
    /* The card is the boot volume. Never power-cycle it on a failed switch:
       restore rails and registers only (T210Power3V3 honours this flag). */
    T210SoftMode = TRUE;
#endif
#if NV_T210_UHS_DMA
    T210DmaInit(s);
#endif
    s->T210SlotStart = KeQueryInterruptTime();
    s->T210Ready = TRUE;
    return STATUS_SUCCESS;
}

static VOID T210Release(PSDHC_EXTENSION s)
{
    if (s->T210Car) { MmUnmapIoSpace(s->T210Car, 0x1000); s->T210Car = NULL; }
    if (s->T210Misc) { MmUnmapIoSpace(s->T210Misc, 0x4000); s->T210Misc = NULL; }
    if (s->T210Pmc) { MmUnmapIoSpace(s->T210Pmc, 0x1000); s->T210Pmc = NULL; }
    if (s->T210I2c) { MmUnmapIoSpace(s->T210I2c, 0x1000); s->T210I2c = NULL; }
    if (s->T210I2cAlt) { MmUnmapIoSpace(s->T210I2cAlt, 0x1000); s->T210I2cAlt = NULL; }
    if (s->T210Gpio) { MmUnmapIoSpace(s->T210Gpio, 0x1000); s->T210Gpio = NULL; }
#if NV_T210_UHS_DMA
    if (s->T210Dma) { MmFreeContiguousMemory(s->T210Dma); s->T210Dma = NULL; s->T210DmaCapacity = 0; }
#endif
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

/* ------------------------------------------------- CMD6 SWITCH_FUNC query */

/* SD Simplified Spec: CMD6 argument = Mode<<31 | group6..group1 (4 bits each).
   Mode 0 asks for the function status block ("check function"), 0xF in a group
   means "do not change", so 0x00FFFFF0 is the plain capability query. Mode 1
   (0x80000000 set) selects a function; only group 1 (bus speed) is written
   here, every other group stays at 0xF ("no change"). */
#define T210_CMD6_ARG_QUERY  0x00FFFFF0UL
#define T210_CMD6_ARG_SELECT 0x80FFFFF0UL

/* The 64-byte status block, kept verbatim: the deferred profile only records
   it (the decision is taken with the T214/T210 bus-timing tables, and a raw
   dump is what a later analysis needs). Byte 13 is the group-1 support mask:
   bit0 SDR12, bit1 SDR25/HS, bit2 SDR50, bit3 SDR104, bit4 DDR50. */
typedef struct _T210_SWITCH_STATUS {
    ULONG  Word[16];          /* 64-byte function status block, little endian */
    ULONG  R1;                /* card status from the R1 response */
    USHORT InterruptStatus;
    USHORT ErrorStatus;
    USHORT PresentState;
    USHORT Reserved;
    NTSTATUS Status;
} T210_SWITCH_STATUS, *PT210_SWITCH_STATUS;

static ULONG
T210SwitchGroup1(
    _In_ const T210_SWITCH_STATUS *Switch
    )
{
    /* Byte 13 of the block lives in Word[3] bits [15:8]. */
    return (Switch->Word[3] >> 8) & 0xffu;
}

/* One CMD6 (SWITCH_FUNC) on the live host controller: Argument selects either
 * the capability query (mode 0) or a function selection (mode 1).
 *
 * This runs while SDPORT owns the controller and the card is the boot volume,
 * so it touches nothing that outlives the command:
 *
 *   - every interrupt source is masked for the duration (the probe polls), and
 *     the status bits it produces are cleared before the masks are restored;
 *   - the DMA select is forced to SDMA and the data moves through the buffer
 *     data port, which is also the only path this profile uses (PIO);
 *   - block size/count, transfer mode and the timeout counter are saved and
 *     written back, so SDPORT's next request starts from its own settings;
 *   - CMD6 mode 0 changes no card state: it only reports the function table.
 *
 * The caller must have confirmed the bus is idle (Command/DAT inhibit clear).
 */
static NTSTATUS
T210Cmd6(
    _In_ PSDHC_EXTENSION s,
    _In_ ULONG Argument,
    _Out_ PT210_SWITCH_STATUS Switch
    )
{
    USHORT SavedTransferMode, SavedBlockSize, SavedBlockCount, SavedTimeout;
    USHORT SavedInterruptEnable, SavedErrorEnable;
    USHORT SavedInterruptSignal, SavedErrorSignal;
    UCHAR SavedHostControl;
    USHORT Events = 0;
    ULONG i;
    NTSTATUS st;

    RtlZeroMemory(Switch, sizeof(*Switch));

    if (SdhcReadRegisterUlong(s, SDHC_PRESENT_STATE) &
        (SDHC_PS_CMD_INHIBIT | SDHC_PS_DAT_INHIBIT)) {
        Switch->Status = STATUS_DEVICE_BUSY;
        return STATUS_DEVICE_BUSY;
    }

    SavedTransferMode = SdhcReadRegisterUshort(s, SDHC_TRANSFER_MODE);
    SavedBlockSize = SdhcReadRegisterUshort(s, SDHC_BLOCK_SIZE);
    SavedBlockCount = SdhcReadRegisterUshort(s, SDHC_BLOCK_COUNT);
    SavedTimeout = SdhcReadRegisterUshort(s, SDHC_TIMEOUT_CONTROL);
    SavedInterruptEnable = SdhcReadRegisterUshort(s, SDHC_INTERRUPT_STATUS_ENABLE);
    SavedErrorEnable = SdhcReadRegisterUshort(s, SDHC_ERROR_STATUS_ENABLE);
    SavedInterruptSignal = SdhcReadRegisterUshort(s, SDHC_INTERRUPT_SIGNAL_ENABLE);
    SavedErrorSignal = SdhcReadRegisterUshort(s, SDHC_ERROR_SIGNAL_ENABLE);
    SavedHostControl = SdhcReadRegisterUchar(s, SDHC_HOST_CONTROL);

    /* Mute and clear: SDPORT's ISR/DPC must not see this command at all. */
    SdhcWriteRegisterUshort(s, SDHC_INTERRUPT_SIGNAL_ENABLE, 0);
    SdhcWriteRegisterUshort(s, SDHC_ERROR_SIGNAL_ENABLE, 0);
    SdhcWriteRegisterUshort(s, SDHC_INTERRUPT_STATUS, 0xffff);
    SdhcWriteRegisterUshort(s, SDHC_ERROR_STATUS, 0xffff);

    SdhcWriteRegisterUchar(s, SDHC_HOST_CONTROL,
                           (UCHAR)(SavedHostControl & ~SDHC_HC_DMA_SELECT_MASK));
    SdhcWriteRegisterUshort(s, SDHC_TIMEOUT_CONTROL, SDHC_TC_MAX_DATA_TIMEOUT);
    SdhcWriteRegisterUshort(s, SDHC_BLOCK_SIZE, 64);
    SdhcWriteRegisterUshort(s, SDHC_BLOCK_COUNT, 1);
    SdhcWriteRegisterUshort(s, SDHC_TRANSFER_MODE, SDHC_TM_TRANSFER_READ);
    SdhcWriteRegisterUlong(s, SDHC_ARGUMENT, Argument);
    SdhcWriteRegisterUshort(s, SDHC_COMMAND,
                            (USHORT)((6u << 8) |
                                     SDHC_CMD_RESPONSE_48BIT_NOBUSY |
                                     SDHC_CMD_CRC_CHECK_ENABLE |
                                     SDHC_CMD_INDEX_CHECK_ENABLE |
                                     SDHC_CMD_DATA_PRESENT));

    /* One block of 64 bytes at the current (48 MHz, 3.3 V HS) clock: a read
       buffer ready event follows the R1 response, transfer complete ends it.
       The poll interval is kept short on purpose: SDPORT may start its own
       request at any time, so this command's window on the bus is ~1 ms at
       worst and typically a few hundred microseconds. */
    for (i = 0; i < 2000; ++i) {           /* at most ~100 ms */
        Events = SdhcReadRegisterUshort(s, SDHC_INTERRUPT_STATUS);
        if (Events & (SDHC_IS_BUFFER_READ_READY | SDHC_IS_ERROR_INTERRUPT |
                      SDHC_IS_TRANSFER_COMPLETE)) {
            break;
        }
        KeStallExecutionProcessor(50);
    }

    Switch->R1 = SdhcReadRegisterUlong(s, SDHC_RESPONSE);
    Switch->PresentState = (USHORT)SdhcReadRegisterUlong(s, SDHC_PRESENT_STATE);

    if (Events & SDHC_IS_ERROR_INTERRUPT) {
        st = STATUS_IO_DEVICE_ERROR;
    } else if (!(Events & SDHC_IS_BUFFER_READ_READY)) {
        st = STATUS_IO_TIMEOUT;
    } else {
        SdhcReadDataPort(s, (PUCHAR)Switch->Word, sizeof(Switch->Word));
        /* Wait for the end of the data phase before the restore below. */
        for (i = 0; i < 2000 && !(Events & SDHC_IS_TRANSFER_COMPLETE); ++i) {
            Events = SdhcReadRegisterUshort(s, SDHC_INTERRUPT_STATUS);
            if (Events & SDHC_IS_ERROR_INTERRUPT) break;
            KeStallExecutionProcessor(50);
        }
        st = (Events & SDHC_IS_ERROR_INTERRUPT) ? STATUS_IO_DEVICE_ERROR :
             ((Events & SDHC_IS_TRANSFER_COMPLETE) ? STATUS_SUCCESS : STATUS_IO_TIMEOUT);
    }

    Switch->InterruptStatus = SdhcReadRegisterUshort(s, SDHC_INTERRUPT_STATUS);
    Switch->ErrorStatus = SdhcReadRegisterUshort(s, SDHC_ERROR_STATUS);

    /* Leave the controller exactly as SDPORT left it. */
    SdhcWriteRegisterUshort(s, SDHC_INTERRUPT_STATUS, 0xffff);
    SdhcWriteRegisterUshort(s, SDHC_ERROR_STATUS, 0xffff);
    SdhcWriteRegisterUshort(s, SDHC_BLOCK_SIZE, SavedBlockSize);
    SdhcWriteRegisterUshort(s, SDHC_BLOCK_COUNT, SavedBlockCount);
    SdhcWriteRegisterUshort(s, SDHC_TRANSFER_MODE, SavedTransferMode);
    SdhcWriteRegisterUshort(s, SDHC_TIMEOUT_CONTROL, SavedTimeout);
    SdhcWriteRegisterUchar(s, SDHC_HOST_CONTROL, SavedHostControl);
    SdhcWriteRegisterUshort(s, SDHC_INTERRUPT_STATUS_ENABLE, SavedInterruptEnable);
    SdhcWriteRegisterUshort(s, SDHC_ERROR_STATUS_ENABLE, SavedErrorEnable);
    SdhcWriteRegisterUshort(s, SDHC_INTERRUPT_SIGNAL_ENABLE, SavedInterruptSignal);
    SdhcWriteRegisterUshort(s, SDHC_ERROR_SIGNAL_ENABLE, SavedErrorSignal);

    if (NT_SUCCESS(st) && Switch->ErrorStatus != 0) st = STATUS_IO_DEVICE_ERROR;
    Switch->Status = st;
    return st;
}

/* Ask the card one CMD6 query right after the 1.8 V switch and report the
   result as a bugcheck: 0xDA08 = the card still answers, 0xDA09 = it does not.
   Only used by bring-up builds; the answer decides whether the boot failure
   sits in the electrical switch or in whatever SDPORT does afterwards. */
#if NV_T210_UHS_POSTCHECK || NV_T210_UHS_POSTCHECK2
static VOID
T210PostCheck(
    _In_ PSDHC_EXTENSION s
    )
{
    T210_SWITCH_STATUS Fn;
    NTSTATUS st;

    RtlZeroMemory(&Fn, sizeof(Fn));
    st = T210Cmd6(s, T210_CMD6_ARG_QUERY, &Fn);
    T210Stop(NT_SUCCESS(st) ? T210_STOP_POST_OK : T210_STOP_POST_FAIL);
}
#endif

/* Select a bus speed function (CMD6 mode 1, group 1 only). The caller decides
 * whether the mode is safe to ask for: this is the same command SDPORT uses. */
static NTSTATUS
T210Cmd6Select(
    _In_ PSDHC_EXTENSION s,
    _In_ ULONG AccessMode,
    _Out_ PT210_SWITCH_STATUS Switch
    )
{
    return T210Cmd6(s, T210_CMD6_ARG_SELECT | (AccessMode & 0xfu), Switch);
}

/* A no-data command on the live controller, same masking rules as CMD6.
 *
 * OCR (CMD58, R3) is the second, independent UHS-I statement: bit 24 (S18A)
 * says the card accepts 1.8 V signalling. It costs no data transfer, so it
 * also cross-checks the command path itself. */
typedef struct _T210_CMD_RESULT {
    ULONG  Response;
    USHORT InterruptStatus;
    USHORT ErrorStatus;
    NTSTATUS Status;
} T210_CMD_RESULT, *PT210_CMD_RESULT;

static NTSTATUS
T210SendCommand(
    _In_ PSDHC_EXTENSION s,
    _In_ ULONG Index,
    _In_ ULONG Argument,
    _In_ USHORT ResponseType,      /* SDHC_CMD_RESPONSE_48BIT_NOBUSY or _WBUSY */
    _In_ BOOLEAN CheckCrcIndex,
    _Out_ PT210_CMD_RESULT Result
    )
{
    USHORT SavedTransferMode, SavedInterruptEnable, SavedErrorEnable;
    USHORT SavedInterruptSignal, SavedErrorSignal;
    USHORT Events = 0;
    ULONG i;
    NTSTATUS st;

    RtlZeroMemory(Result, sizeof(*Result));
    if (SdhcReadRegisterUlong(s, SDHC_PRESENT_STATE) &
        (SDHC_PS_CMD_INHIBIT | SDHC_PS_DAT_INHIBIT)) {
        Result->Status = STATUS_DEVICE_BUSY;
        return STATUS_DEVICE_BUSY;
    }

    SavedTransferMode = SdhcReadRegisterUshort(s, SDHC_TRANSFER_MODE);
    SavedInterruptEnable = SdhcReadRegisterUshort(s, SDHC_INTERRUPT_STATUS_ENABLE);
    SavedErrorEnable = SdhcReadRegisterUshort(s, SDHC_ERROR_STATUS_ENABLE);
    SavedInterruptSignal = SdhcReadRegisterUshort(s, SDHC_INTERRUPT_SIGNAL_ENABLE);
    SavedErrorSignal = SdhcReadRegisterUshort(s, SDHC_ERROR_SIGNAL_ENABLE);

    SdhcWriteRegisterUshort(s, SDHC_INTERRUPT_SIGNAL_ENABLE, 0);
    SdhcWriteRegisterUshort(s, SDHC_ERROR_SIGNAL_ENABLE, 0);
    SdhcWriteRegisterUshort(s, SDHC_INTERRUPT_STATUS, 0xffff);
    SdhcWriteRegisterUshort(s, SDHC_ERROR_STATUS, 0xffff);
    SdhcWriteRegisterUshort(s, SDHC_TRANSFER_MODE,
                            (USHORT)(SavedTransferMode & ~(SDHC_TM_DMA_ENABLE |
                                                           SDHC_TM_AUTO_CMD12_ENABLE |
                                                           SDHC_TM_AUTO_CMD23_ENABLE |
                                                           SDHC_TM_MULTIBLOCK |
                                                           SDHC_TM_TRANSFER_READ)));

    SdhcWriteRegisterUlong(s, SDHC_ARGUMENT, Argument);
    SdhcWriteRegisterUshort(s, SDHC_COMMAND,
                            (USHORT)(((Index & 0x3fu) << 8) |
                                     ResponseType |
                                     (CheckCrcIndex ?
                                      (SDHC_CMD_CRC_CHECK_ENABLE |
                                       SDHC_CMD_INDEX_CHECK_ENABLE) : 0)));

    for (i = 0; i < 2000; ++i) {
        Events = SdhcReadRegisterUshort(s, SDHC_INTERRUPT_STATUS);
        if (Events & (SDHC_IS_CMD_COMPLETE | SDHC_IS_ERROR_INTERRUPT)) break;
        KeStallExecutionProcessor(50);
    }

    Result->Response = SdhcReadRegisterUlong(s, SDHC_RESPONSE);
    Result->InterruptStatus = SdhcReadRegisterUshort(s, SDHC_INTERRUPT_STATUS);
    Result->ErrorStatus = SdhcReadRegisterUshort(s, SDHC_ERROR_STATUS);
    st = (Result->ErrorStatus != 0) ? STATUS_IO_DEVICE_ERROR :
         ((Events & SDHC_IS_CMD_COMPLETE) ? STATUS_SUCCESS : STATUS_IO_TIMEOUT);

    SdhcWriteRegisterUshort(s, SDHC_INTERRUPT_STATUS, 0xffff);
    SdhcWriteRegisterUshort(s, SDHC_ERROR_STATUS, 0xffff);
    SdhcWriteRegisterUshort(s, SDHC_TRANSFER_MODE, SavedTransferMode);
    SdhcWriteRegisterUshort(s, SDHC_INTERRUPT_STATUS_ENABLE, SavedInterruptEnable);
    SdhcWriteRegisterUshort(s, SDHC_ERROR_STATUS_ENABLE, SavedErrorEnable);
    SdhcWriteRegisterUshort(s, SDHC_INTERRUPT_SIGNAL_ENABLE, SavedInterruptSignal);
    SdhcWriteRegisterUshort(s, SDHC_ERROR_SIGNAL_ENABLE, SavedErrorSignal);

    Result->Status = st;
    return st;
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

#pragma clang diagnostic pop

#endif /* T210_UHS_H */








