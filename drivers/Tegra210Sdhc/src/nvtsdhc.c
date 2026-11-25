/*++

Copyright (c) 2002-2014  Microsoft Corporation

Module Name:

    sdhc.c

Abstract:

    This file contains the code that interfaces with standard host controller
    implementations.

Author:

    Greg Garbern (greggar) 01-May-2014

Environment:

    Kernel mode only.

Revision History:

    Greg Garbern (greggar) 12-June-2014

--*/

#include <ntddk.h>
#include <sdport.h>
#include "nvtsdhc.h"

// The Microsoft sample this is based on keeps a few locals it only assigns.
#pragma clang diagnostic ignored "-Wunused-but-set-variable"

// Nintendo Switch (Tegra210) bring-up policy.
// The default profile retains 3.3 V timing. The bounded T214 SDR104 profile
// supplies the board-specific voltage, tuning and DMA implementation below.
#define NV_T210_SAFE_BRINGUP (!NV_T214_SDR104)
#if NV_T214_SDR104 && !NV_T210_LITE_WINARM_WRITE
#error "SDR104 requires the bounded per-card Lite profile."
#endif

// Opt-in build for reading deployment media on the Hekate-booted HDH-001.
#ifndef NV_T210_LITE_READ_ONLY
#define NV_T210_LITE_READ_ONLY 0
#endif
#if NV_T210_LITE_READ_ONLY && NV_T210_LITE_WINARM_WRITE
#error "Choose one Lite SD profile for this package."
#endif
#define NV_T210_LITE_SLOT (NV_T210_LITE_READ_ONLY || NV_T210_LITE_WINARM_WRITE)

#if NV_T210_LITE_WINARM_WRITE
#include "winarm-target.h"

enum {
    LiteSdxc = 1, LiteGpt = 2, LitePartition = 4, LiteNtfs = 8, LiteMbr = 16,
    LiteFatBoot = 32,
    LiteWriteFacts = LiteSdxc | LiteMbr | LiteNtfs,
    LiteFatWriteFacts = LiteSdxc | LiteMbr | LiteFatBoot
};

static VOID LiteForgetCard(PSDHC_EXTENSION slot)
{
    InterlockedExchange(&slot->TargetFacts, 0);
    slot->NextReadLba = MAXULONG;
    slot->AppPrefixAccepted = FALSE;
    slot->IssuedWriteCount = 0;
#if NV_T214_DDR200
    slot->T214CardInfoValid = 0;
    slot->T214Rca = 0;
#endif
}

static VOID LiteObserveRead(PSDHC_EXTENSION slot, PSDPORT_COMMAND cmd)
{
    ULONG lba = slot->NextReadLba;
    if (lba == MAXULONG || cmd->BlockSize != 512 || cmd->DataBuffer == NULL) {
        return;
    }
    slot->NextReadLba = lba + 1;
    LONG bit;
    BOOLEAN matches;
    if (lba == 0) {
        bit = LiteMbr;
        matches = (BOOLEAN)LiteSelectWindowsPartition(cmd->DataBuffer);
    } else if (lba == 1) {
        bit = LiteGpt;
        matches = (BOOLEAN)LiteIsHeader(cmd->DataBuffer);
    } else if (lba == 4) {
        bit = LitePartition;
        matches = (BOOLEAN)LiteIsPartition(cmd->DataBuffer);
    } else if (lba == WINARM_FIRST) {
        bit = LiteNtfs;
        matches = (BOOLEAN)LiteIsBootSector(cmd->DataBuffer);
#ifdef FAT32_FIRST
    } else if (lba == FAT32_FIRST) {
        bit = LiteFatBoot;
        matches = (BOOLEAN)LiteIsFatBootSector(cmd->DataBuffer);
#endif
    } else {
        return;
    }
    if (matches) {
        InterlockedOr(&slot->TargetFacts, bit);
    } else {
        InterlockedAnd(&slot->TargetFacts, ~bit);
    }
}

/* DMA returns a complete range at once. Only the partition-table and volume
 * boot sectors affect the write guard; ordinary FAT/NTFS data need no
 * per-sector dispatch at DPC level. Keep the same identity checks in one place. */
static VOID LiteObserveReadRange(PSDHC_EXTENSION slot, PSDPORT_COMMAND cmd, ULONG blocks)
{
    const ULONG interesting[] = {0, 1, 4, WINARM_FIRST,
#ifdef FAT32_FIRST
        FAT32_FIRST,
#endif
    };
    ULONG first = slot->NextReadLba;
    if (first == MAXULONG || blocks == 0 || cmd->BlockSize != 512 || cmd->DataBuffer == NULL) return;
    for (ULONG i = 0; i < RTL_NUMBER_OF(interesting); ++i) {
        ULONG lba = interesting[i];
        if (lba >= first && lba - first < blocks) {
            SDPORT_COMMAND one = *cmd;
            one.DataBuffer = cmd->DataBuffer + (SIZE_T)(lba - first) * 512;
            slot->NextReadLba = lba;
            LiteObserveRead(slot, &one);
        }
    }
    slot->NextReadLba = blocks > MAXULONG - first ? MAXULONG : first + blocks;
}

static BOOLEAN LiteBlocksInExtent(ULONG lba, ULONG count, ULONG first, ULONG length)
{
    return count != 0 && lba >= first && lba - first < length &&
           count <= length - (lba - first);
}

static BOOLEAN LiteAllowRequest(PSDHC_EXTENSION slot, PSDPORT_REQUEST request)
{
    PSDPORT_COMMAND cmd = &request->Command;
    // SDPORT sends CMD55 -> ACMD42(0) during four-bit bus setup. ACMD42
    // changes the DAT3 pull-up. The separate standard CMD42 carries lock data.
    if (cmd->Index == 42) {
        return slot->AppPrefixAccepted && request->Type == SdRequestTypeCommandNoTransfer &&
               cmd->Class == SdCommandClassApp && cmd->ResponseType == SdResponseTypeR1 &&
               cmd->TransferType == SdTransferTypeNone && cmd->BlockCount == 0 &&
               cmd->TransferDirection != SdTransferDirectionWrite && cmd->Argument <= 1;
    }
    if (cmd->Index == 24 || cmd->Index == 25) {
        ULONG lba = cmd->Argument;
        ULONG count = cmd->BlockCount;
        LONG required;
        if (LiteBlocksInExtent(lba, count, WINARM_FIRST, WINARM_COUNT)) {
            required = LiteWriteFacts;
#ifdef FAT32_FIRST
        } else if (LiteBlocksInExtent(lba, count, FAT32_FIRST, FAT32_COUNT)) {
            required = LiteFatWriteFacts;
#endif
        } else {
            return FALSE;
        }
        if ((slot->TargetFacts & required) != required ||
            slot->CrashdumpMode || slot->AppPrefixAccepted ||
            cmd->Class == SdCommandClassApp || cmd->TransferDirection != SdTransferDirectionWrite ||
            cmd->TransferMethod != SdTransferMethodPio || cmd->BlockSize != 512 ||
            count == 0 || (cmd->Index == 24 && count != 1) ||
            (cmd->Index == 24 && cmd->TransferType != SdTransferTypeSingleBlock) ||
            (cmd->Index == 25 && cmd->TransferType != SdTransferTypeMultiBlock &&
                                 cmd->TransferType != SdTransferTypeMultiBlockNoStop)) {
            return FALSE;
        }
        if (request->Type == SdRequestTypeCommandWithTransfer) {
            return TRUE;
        }
        return request->Type == SdRequestTypeStartTransfer && cmd->DataBuffer != NULL &&
               cmd->Index == slot->IssuedWriteIndex && lba == slot->IssuedWriteLba &&
               count <= slot->IssuedWriteCount;
    }
    if ((request->Type == SdRequestTypeCommandWithTransfer || request->Type == SdRequestTypeStartTransfer) &&
        cmd->TransferDirection != SdTransferDirectionRead) {
        return FALSE;
    }
    switch (cmd->Index) {
    case 0: case 1: case 2: case 3: case 5: case 6: case 7: case 8: case 9: case 10:
    case 11: case 12: case 13: case 15: case 16: case 17: case 18: case 19: case 22:
    case 41: case 51: case 55:
        return TRUE;
    case 52: case 53:
        return (cmd->Argument & 0x80000000u) == 0;
    default:
        return FALSE;
    }
}
#endif

// Move all data with PIO, never DMA. On Windows 11 22621 the inbox sdbus could
// not read the boot volume with this non-coherent controller (_CCA = 0), and
// pretending it is coherent (_CCA = 1) hung the boot. PIO avoids DMA and cache
// maintenance entirely; it is slower but enough to boot.
#ifndef NV_T210_PIO_ONLY
#define NV_T210_PIO_ONLY 0
#endif

// 0.3.0: PIO tops out near 5 MB/s (each 4-byte data port read costs ~0.8 us),
// so move data with ADMA2, 32-bit addressing only. The DMA adapter then bounces
// buffers above 4 GB into low memory, and 64-bit ADMA (which Tegra210 runs in
// host v4 mode under L4T) and its descriptor format are not used. Cache
// maintenance comes from _CCA = 0. Crashdump mode keeps PIO.
#define NV_T210_ADMA32 1

// SDMMC1 input clock from the CAR: PLLP (408 MHz, source 0) divided by
// (divisor field + 2). The old assumption was "field 15 => 8.5 => 48 MHz", but
// the hardware divider term is (field + 2), so field 15 is 408/17 = 24 MHz, and
// the host divider is SDCLK = base / (2 * SELFREQ) - so SELFREQ 0 was 12 MHz on
// the wire, not 48. That is what capped reads at 5.5 MiB/s.
// NV_T210_CAR_DIVISOR overrides the field: 0 means PLLP/2 = 204 MHz of headroom,
// with the real rate reported as the base clock so SDPORT's request is honoured.
#define NV_T210_CAR_DIVISOR_NONE 0xFFFFFFFFu
#ifndef NV_T210_FIXED_CAR_CLOCK
#define NV_T210_FIXED_CAR_CLOCK 1
#endif
#define NV_T210_CAR_BASE            0x60006000
#define NV_T210_CLK_SOURCE_SDMMC1   0x150
#define NV_T210_SDMMC1_CAR_SOURCE   ((0u << 29) | 15u)
#define NV_T210_SDMMC1_CAR_KHZ      48000
#define NV_T210_PLLP_KHZ            408000
#ifndef NV_T210_CAR_DIVISOR
#define NV_T210_CAR_DIVISOR         NV_T210_CAR_DIVISOR_NONE
#endif

/* Build-profile defaults for the T210 UHS / DMA options (overridden by -D). */
#ifndef NV_T210_UHS_NO_SDR104
#define NV_T210_UHS_NO_SDR104 0
#endif
#ifndef NV_T210_UHS_NO_ADVERTISE
#define NV_T210_UHS_NO_ADVERTISE 0
#endif
#ifndef NV_T210_UHS_SLOT_CAR
#define NV_T210_UHS_SLOT_CAR 0
#endif
#ifndef NV_T210_UHS_HOLD_18V
#define NV_T210_UHS_HOLD_18V 0
#endif
#ifndef NV_T210_UHS_TUNE_AT_SPEED
#define NV_T210_UHS_TUNE_AT_SPEED 0
#endif

// Boot diagnostics. Nothing (debugger, boot log, dump) survives a failed boot
// from the SD this driver serves, so if the slot has not read any data a few
// seconds after the driver starts, stop the machine with a code saying how far
// it got. The stop code is shown as hex on the blue screen:
//
//   0xD S F CC EE K   (stages 0-2 only: once commands succeed the card is coming up)
//      S  stage: 0 slot never initialized, 1 no command issued,
//                2 no command succeeded, 3 commands OK but no data read,
//                4 some data read (fewer than NV_DIAG_OK_BLOCKS blocks)
//      F  flags: 1 card-inserted bit, 2 card-stable bit (present state at
//                initialize), 4 an interrupt was seen, 8 host reset failed
//      CC last command index issued
//      EE command index of the last error (FF: none)
//      K  last error: 0 none, 1 command timeout, 2 command CRC, 3 command end
//         bit/index, 4 data timeout, 5 data CRC, 6 data end bit, 7 other,
//         8 the request could not be issued
#ifndef NV_DIAG_STOP
#define NV_DIAG_STOP (!NV_T210_LITE_SLOT)
#endif
#define NV_DIAG_DELAY_MS 2500
#define NV_DIAG_OK_BLOCKS 8

typedef struct _NV_DIAG {
    KTIMER Timer;
    KDPC Dpc;
    BOOLEAN Initialized;
    ULONG PresentState;
    ULONG Flags;
    UCHAR LastCommand;
    UCHAR LastErrorCommand;
    UCHAR LastErrorKind;
    ULONG LastErrors;
    ULONG Commands;
    ULONG CommandsOk;
    ULONG BlocksRead;

    // Written to <service>\Parameters\Diag every NV_DIAG_REG_SECONDS once
    // Windows is running, to see how the slot performs.
    PVOID Extension;
    ULONG ClockKhzRequested;
    ULONG ClockKhzActual;
    ULONG BusWidth;
    ULONG BusSpeed;
    ULONG Interrupts;
    ULONG ErrorInterrupts;
    ULONG CommandErrors;
    ULONG BlocksWritten;
#if NV_T210_LITE_WINARM_WRITE
    ULONG App42Done;
    ULONG DeniedCount;
    ULONG LastDeniedCommand;
    ULONG LastDeniedArgument;
    ULONG DeniedShape;
    LONG SnapshotFacts;
#endif

    // Register snapshots taken where the driver already touches the host. The
    // diag thread must not read the host itself: when the port powers it down,
    // a register read is a bus error (WHEA_UNCORRECTABLE_ERROR, seen in 0.2.3).
    ULONG RegCapabilities;
    ULONG RegCapabilities2;
    ULONG RegClockControl;
    ULONG RegHostControl;
    ULONG CarSdmmc1Before;
    ULONG CarSdmmc1After;

    // Data commands, timed from issue to completion (DataUs) and from the
    // previous completion to the next issue (GapUs): bus time against time
    // spent above the miniport. 0.3.0 read 5 MB/s at 48 MHz, 4 bit.
    PVOID DataRequest;
    LONGLONG DataStart;
    LONGLONG DataEnd;
    ULONG DataStartBlocks;
    ULONG DataCommands;
    ULONG DataBlocks;
    ULONG DataUs;
    ULONG GapUs;

    // PIO decomposition, sampled inside SdhcStartPioTransfer at block
    // granularity: upper-layer bookkeeping (DataUs/GapUs above) cannot be
    // trusted because the port reuses its request objects. Ticks are raw
    // KeQueryPerformanceCounter units and are converted when the key is saved.
    ULONG PioCalls;
    ULONG PioCallBlocks;
    ULONG PioCallMore;
    ULONG PioReadyMiss;
    ULONG PioBudgetHit;
    ULONG PioPollIters;
    LONGLONG PioPollTicks;
    LONGLONG PioPortTicks;
    LONGLONG PioCallTicks;
    /* KeQueryInterruptTime() of the last completed data transfer: the DMA probe
       only runs when the disk has been idle for seconds, which boot never is. */
    LONGLONG LastDataIt;

    /* SG-DMA (SDPORT scatter/gather with device-visible addresses) traffic and
       errors: shows whether ADMA really moves the blocks and what the
       controller reports when it does not. */
    ULONG SgDmaRequests;
    ULONG SgDmaBlocks;
    ULONG SgDmaErrors;
    ULONG SgDmaErrStatus;
    ULONG SgDmaAdmaErr;
    WCHAR RegistryPath[256];
} NV_DIAG;

#define NV_DIAG_REG_SECONDS 10

// The registry diag thread (0.2.2) and the buffered PIO (0.2.2) are off:
// 0.2.3-0.2.5 stopped with WHEA_UNCORRECTABLE_ERROR and 0.2.1, which had
// neither, ran to the desktop. Turn them back on one at a time to find out.
#ifndef NV_DIAG_REG
#define NV_DIAG_REG 0
#endif
#ifndef NV_PIO_BUFFERED
#define NV_PIO_BUFFERED 0
#endif

/* NV_T210_FAST_PIO=1 moves the buffer data port with an inlined
   READ_REGISTER_ULONG loop instead of one SDPORT helper call per 4 bytes.
   Same order of accesses (the data port is a FIFO: the same address is read
   repeatedly), only the per-dword call overhead disappears. */
#ifndef NV_T210_FAST_PIO
#define NV_T210_FAST_PIO 0
#endif

// PIO: after one buffer-ready interrupt, keep moving blocks while the present
// state says the buffer is ready (as Linux sdhci does), waiting up to
// NV_PIO_POLL_US for each next block, at most NV_PIO_POLL_BLOCKS per call.
// One interrupt round trip per 512-byte block held reads at 5.4 MB/s (0.2.8).
#ifndef NV_PIO_POLL
#define NV_PIO_POLL 1
#endif
#define NV_PIO_POLL_US 200
/* Blocks moved per miniport call. SDPORT hands out ~55-block commands, so 32
   forced at least one interrupt round trip per command; 128 lets one call drain
   the whole command. Override with -D NV_PIO_POLL_BLOCKS=<n>. */
#ifndef NV_PIO_POLL_BLOCKS
#define NV_PIO_POLL_BLOCKS 32
#endif

// Clock and bus settings written to <service>\Parameters\Diag when the port
// sets them: a queued worker writes a copied snapshot, never in a port
// callback or crashdump mode. No timer, flush, or extra hardware register reads.
#ifndef NV_DIAG_CLOCK
#define NV_DIAG_CLOCK 1
#endif

static NV_DIAG NvDiag = { .LastErrorCommand = 0xFF };

/* Microsecond time base for the PIO decomposition: KeQueryPerformanceCounter is
   the cheap counter, legal at DISPATCH_LEVEL, and its frequency is cached on
   first use. Zeros mean "no time base", never a bogus measurement. */
static LONGLONG NvPioFreq;

static LONGLONG
NvPioNow(
    VOID
    )
{
    LARGE_INTEGER Frequency;
    LARGE_INTEGER Now;

    Now = KeQueryPerformanceCounter(&Frequency);
    if (Frequency.QuadPart != 0) {
        NvPioFreq = Frequency.QuadPart;
    }
    return Now.QuadPart;
}

static ULONG
NvPioUs(
    _In_ LONGLONG Ticks
    )
{
    LONGLONG Unit;

    if ((NvPioFreq <= 0) || (Ticks <= 0)) {
        return 0;
    }
    Unit = NvPioFreq / 1000000;
    if (Unit <= 0) {
        Unit = 1;
    }
    return (ULONG) (Ticks / Unit);
}

#if NV_T214_SDR104
#include "t214_sdmmc.h"
#endif
#if NV_T210_UHS
#include "t210_uhs.h"
#endif
#if NV_T214_DDR200
#include "t214_ddr200.h"

/* Observe only completed single-block metadata reads already issued by
 * SDPORT. Do not infer capability from a card name or send private commands
 * inside the request-completion path.
 */
static VOID T214ObserveCardInfo(PSDHC_EXTENSION s, PSDPORT_REQUEST request)
{
    PSDPORT_COMMAND c = &request->Command;
    if (request->Type != SdRequestTypeStartTransfer ||
        c->TransferDirection != SdTransferDirectionRead ||
        c->TransferType != SdTransferTypeSingleBlock || c->BlockCount != 0 ||
        c->DataBuffer == NULL) return;
    if (c->Index == 51 && c->Class == SdCommandClassApp &&
        c->BlockSize == 8 && c->Length == 8) {
        RtlCopyMemory(s->T214Scr, c->DataBuffer, 8);
        s->T214CardInfoValid |= 1;
    } else if (c->Index == 6 && c->Class == SdCommandClassStandard &&
               c->BlockSize == 64 && c->Length == 64) {
        RtlCopyMemory(s->T214SwitchStatus, c->DataBuffer, 64);
        s->T214CardInfoValid |= 2;
    }
}
#endif

static UCHAR
NvDiagErrorKind(
    _In_ ULONG Errors
    )
{
    if (Errors & 0x1) return 1;
    if (Errors & 0x2) return 2;
    if (Errors & 0xC) return 3;
    if (Errors & 0x10) return 4;
    if (Errors & 0x20) return 5;
    if (Errors & 0x40) return 6;
    return 7;
}

#if NV_DIAG_STOP
static VOID
NvDiagDpc(
    _In_ PKDPC Dpc,
    _In_opt_ PVOID Context,
    _In_opt_ PVOID Arg1,
    _In_opt_ PVOID Arg2
    )
{
    ULONG Stage;
    ULONG Code;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Arg1);
    UNREFERENCED_PARAMETER(Arg2);

    if (NvDiag.BlocksRead >= NV_DIAG_OK_BLOCKS) {
        return;
    }

    if (!NvDiag.Initialized) {
        Stage = 0;
    } else if (NvDiag.Commands == 0) {
        Stage = 1;
    } else if (NvDiag.CommandsOk == 0) {
        Stage = 2;
    } else if (NvDiag.BlocksRead == 0) {
        Stage = 3;
    } else {
        Stage = 4;
    }

    // Commands are completing, so the card is coming up; card init alone can
    // take longer than NV_DIAG_DELAY_MS (seen: 0xD4706051, stopped at CMD6).
    if (Stage >= 3) {
        return;
    }

    Code = 0xD0000000 |
           (Stage << 24) |
           ((NvDiag.Flags & 0xF) << 20) |
           ((ULONG) NvDiag.LastCommand << 12) |
           ((ULONG) NvDiag.LastErrorCommand << 4) |
           (NvDiag.LastErrorKind & 0xF);

    KeBugCheckEx(Code,
                 NvDiag.PresentState,
                 (NvDiag.CommandsOk << 16) | (NvDiag.Commands & 0xFFFF),
                 NvDiag.LastErrors,
                 NvDiag.BlocksRead);
}
#endif

#if NV_DIAG_REG || NV_DIAG_CLOCK
static VOID
NvDiagSetValue(
    _In_ HANDLE Key,
    _In_ PCWSTR Name,
    _In_ ULONG Value
    )
{
    UNICODE_STRING ValueName;

    RtlInitUnicodeString(&ValueName, Name);
    ZwSetValueKey(Key, &ValueName, 0, REG_DWORD, &Value, sizeof(Value));
}
#endif

#if NV_DIAG_CLOCK
/* Snapshot only from the port callback. The worker owns an immutable copy:
 * registry I/O can re-enter this boot miniport even at PASSIVE_LEVEL, and
 * must never execute synchronously inside a bus/request callback. */
static WORK_QUEUE_ITEM NvDiagSaveItem;
static volatile LONG NvDiagSaveQueued;
static volatile LONG64 NvDiagNextPeriodicSave;
static struct { PCWSTR Name; ULONG Value; } NvDiagSnapshot[160];
static ULONG NvDiagSnapshotCount;

/* Diagnostics must not run while the boot disk is still being enumerated.
 * The old path called NvUhsProbe (MmMapIoSpaceEx of PMC/CAR/APB_MISC) from
 * the port callbacks, i.e. at raised IRQL during boot, and wrote registry.
 * That is what produced the 0xD0000FF0 boot stops on 2026-11-21. Everything
 * now waits for NvDiagLate, which a timer sets after the system is up. */
static volatile LONG NvDiagLate;
static KTIMER NvDiagLateTimer;
static KDPC NvDiagLateDpc;
#define NV_DIAG_LATE_SECONDS 60

static VOID NvDiagCapture(PCWSTR Name, ULONG Value)
{
    NT_ASSERT(NvDiagSnapshotCount < RTL_NUMBER_OF(NvDiagSnapshot));
    /* Never write past the array: an out-of-bounds write here corrupts the
       neighbouring globals and produced 0x7B INACCESSIBLE_BOOT_DEVICE on
       2026-11-12.  Release builds have no assert, so clamp explicitly. */
    if (NvDiagSnapshotCount >= RTL_NUMBER_OF(NvDiagSnapshot)) return;
    NvDiagSnapshot[NvDiagSnapshotCount].Name = Name;
    NvDiagSnapshot[NvDiagSnapshotCount++].Value = Value;
}

/* P0 for the T210 (Erista) UHS/DDR200 work: a read-only snapshot of the
 * registers that decide whether SDR104/DDR200 can be attempted at all.
 * Nothing here writes: no voltage switch, no clock change, no tuning.
 * The values answer three open questions (see DDR200-T210-濠电姷鏁告慨鐑藉极閸涘﹥鍙忛柣鎴ｆ閺嬩線鏌熼梻瀵割槮缁炬儳顭烽弻锝夊箛椤掍焦鍎撶紓浣哄С閸楁娊寮婚悢鍏尖拻閻庨潧澹婂Σ顔剧磽娴ｅ搫啸濠电偐鍋撳Δ鐘靛仦閻楁顭囪箛娑樼鐟滃繘鏁嶉悢鍏尖拺?md):
 *   1. does this SDHCI have the vendor pad-calibration block (0x1e0/0x1e4/0x1ec)
 *      and the DDR200 tap field (0x100[23:16])?
 *   2. is PLLC4 (CAR 0x5a4) at the 998.4 MHz the T214 divider maths needs?
 *   3. are the PMC IO pad and the APB_MISC SDMMC1 pads where T214Signaling()
 *      expects them?
 */
#ifndef NV_UHS_PROBE
#define NV_UHS_PROBE 1
#endif

#if NV_UHS_PROBE
static VOID
NvUhsProbe(
    _In_ PSDHC_EXTENSION SdhcExtension
    )
{
    static volatile LONG Done;
    PHYSICAL_ADDRESS Pa;
    PULONG Car, Pmc, Misc, MiscPad;

    if (InterlockedCompareExchange(&Done, 1, 0) != 0) {
        return;
    }

    NvDiagCapture(L"UhsCaps", SdhcReadRegisterUlong(SdhcExtension, SDHC_CAPABILITIES));
    NvDiagCapture(L"UhsCaps2", SdhcReadRegisterUlong(SdhcExtension, SDHC_CAPABILITIES2));
    NvDiagCapture(L"UhsHc2", SdhcReadRegisterUshort(SdhcExtension, SDHC_HOST_CONTROL2));
    NvDiagCapture(L"UhsHost100", SdhcReadRegisterUlong(SdhcExtension, 0x100));
    NvDiagCapture(L"UhsHost10c", SdhcReadRegisterUlong(SdhcExtension, 0x10c));
    NvDiagCapture(L"UhsHost1c0", SdhcReadRegisterUlong(SdhcExtension, 0x1c0));
    NvDiagCapture(L"UhsHost1c4", SdhcReadRegisterUlong(SdhcExtension, 0x1c4));
    NvDiagCapture(L"UhsHost1e0", SdhcReadRegisterUlong(SdhcExtension, 0x1e0));
    NvDiagCapture(L"UhsHost1e4", SdhcReadRegisterUlong(SdhcExtension, 0x1e4));
    NvDiagCapture(L"UhsHost1ec", SdhcReadRegisterUlong(SdhcExtension, 0x1ec));
    NvDiagCapture(L"UhsHost30", SdhcReadRegisterUlong(SdhcExtension, 0x30));
    NvDiagCapture(L"UhsHost34", SdhcReadRegisterUlong(SdhcExtension, 0x34));
    NvDiagCapture(L"UhsHost38", SdhcReadRegisterUlong(SdhcExtension, 0x38));

    Pa.QuadPart = NV_T210_CAR_BASE;
    Car = (PULONG) MmMapIoSpaceEx(Pa, PAGE_SIZE, PAGE_READONLY | PAGE_NOCACHE);
    if (Car != NULL) {
        NvDiagCapture(L"UhsCar150", Car[NV_T210_CLK_SOURCE_SDMMC1 / 4]);
        NvDiagCapture(L"UhsCar55c", Car[0x55c / 4]);
        NvDiagCapture(L"UhsPllc4", Car[0x5a4 / 4]);
        NvDiagCapture(L"UhsPllc4Ctl", Car[0x5a8 / 4]);
        MmUnmapIoSpace(Car, PAGE_SIZE);
    }

    Pa.QuadPart = 0x7000E000;
    Pmc = (PULONG) MmMapIoSpaceEx(Pa, PAGE_SIZE, PAGE_READONLY | PAGE_NOCACHE);
    if (Pmc != NULL) {
        NvDiagCapture(L"UhsPmcStatus", Pmc[0x438 / 4]);
        NvDiagCapture(L"UhsPmc444", Pmc[0x844 / 4]);
        NvDiagCapture(L"UhsPmc4e4", Pmc[0x8e4 / 4]);
        MmUnmapIoSpace(Pmc, PAGE_SIZE);
    }

    Pa.QuadPart = 0x70000000;
    Misc = (PULONG) MmMapIoSpaceEx(Pa, PAGE_SIZE, PAGE_READONLY | PAGE_NOCACHE);
    if (Misc != NULL) {
        NvDiagCapture(L"UhsMiscA98", Misc[0xa98 / 4]);
        MmUnmapIoSpace(Misc, PAGE_SIZE);
    }

    Pa.QuadPart = 0x70003000;
    MiscPad = (PULONG) MmMapIoSpaceEx(Pa, PAGE_SIZE, PAGE_READONLY | PAGE_NOCACHE);
    if (MiscPad != NULL) {
        NvDiagCapture(L"UhsPad3000", MiscPad[0]);
        NvDiagCapture(L"UhsPad3004", MiscPad[0x04 / 4]);
        NvDiagCapture(L"UhsPad3014", MiscPad[0x14 / 4]);
        MmUnmapIoSpace(MiscPad, PAGE_SIZE);
    }
}
#endif

static VOID
NvDiagSaveWorker(
    _In_ PVOID Context
    )
{
    static ULONG Saves;
    UNICODE_STRING Path;
    OBJECT_ATTRIBUTES Attributes;
    HANDLE Key;
    PSDHC_EXTENSION Ext = (PSDHC_EXTENSION)Context;

#if NV_UHS_PROBE
    /* PASSIVE context: safe place for MmMapIoSpaceEx. Never from a port
     * callback, which can be DISPATCH_LEVEL during boot. */
    NvUhsProbe(Ext);
#endif

    RtlInitUnicodeString(&Path, NvDiag.RegistryPath);
    InitializeObjectAttributes(&Attributes, &Path,
                               OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (NT_SUCCESS(ZwCreateKey(&Key, KEY_SET_VALUE, &Attributes, 0, NULL,
                               REG_OPTION_NON_VOLATILE, NULL))) {
        NvDiagSetValue(Key, L"Saves", ++Saves);
        for (ULONG i = 0; i < NvDiagSnapshotCount; ++i) {
            NvDiagSetValue(Key, NvDiagSnapshot[i].Name, NvDiagSnapshot[i].Value);
        }
        ZwClose(Key);
    }
    InterlockedExchange(&NvDiagSaveQueued, 0);
}

static VOID
NvDiagSaveClock(
    _In_ PSDHC_EXTENSION SdhcExtension
    )
{
    /* Inert until the system has settled: no registry, no MMIO mapping, no
     * work item while the boot disk is still being enumerated. */
    if (NvDiagLate == 0) {
        return;
    }
    if (SdhcExtension->CrashdumpMode || NvDiag.RegistryPath[0] == L'\0' ||
        InterlockedCompareExchange(&NvDiagSaveQueued, 1, 0) != 0) {
        return;
    }

    /* No periodic timer: active I/O may refresh this fixed snapshot at most
     * once per 30 seconds. Clock/identity/error transitions remain explicit. */
    InterlockedExchange64(&NvDiagNextPeriodicSave,
                          (LONG64)KeQueryInterruptTime() + 300000000LL);
    NvDiagSnapshotCount = 0;
#if NV_T214_SDR104
    NvDiagCapture( L"Sdr104Build", 1);
    NvDiagCapture( L"AdmaAvailable", SdhcExtension->T214Bounce != NULL);
    NvDiagCapture( L"Signaling18V", SdhcExtension->T214Voltage18);
    NvDiagCapture( L"VoltageStatus", SdhcExtension->T214VoltageStatus);
    NvDiagCapture( L"VoltageFailures", SdhcExtension->T214VoltageFailures);
    NvDiagCapture( L"Tuned", SdhcExtension->T214Tuned);
    NvDiagCapture( L"TuneStatus", SdhcExtension->T214TuneStatus);
    NvDiagCapture( L"TuningRuns", SdhcExtension->T214TuningRuns);
    NvDiagCapture( L"TuningIterations", SdhcExtension->T214TuningIterations);
    NvDiagCapture( L"TuningTap", SdhcExtension->T214Tap);
    NvDiagCapture( L"PadCalFallbacks", SdhcExtension->T214CalFallbacks);
    NvDiagCapture( L"AdmaBlocks", SdhcExtension->T214DmaBlocks);
    NvDiagCapture( L"AdmaBufferBytes", SdhcExtension->T214DmaCapacity);
    NvDiagCapture( L"AdmaRequests", SdhcExtension->T214DmaRequests);
    NvDiagCapture( L"AdmaMaxBlocks", SdhcExtension->T214DmaMaxBlocks);
    NvDiagCapture( L"PioFallbackRequests", SdhcExtension->T214PioFallbacks);
    NvDiagCapture( L"AdmaErrors", SdhcExtension->T214DmaErrors);
    NvDiagCapture( L"SmmuSdmmc1", SdhcExtension->T214Smmu);
    NvDiagCapture( L"ContextSaves", SdhcExtension->T214Saves);
    NvDiagCapture( L"ContextRestores", SdhcExtension->T214Restores);
    NvDiagCapture( L"ResumeStatus", SdhcExtension->T214ResumeStatus);
#if NV_T214_DDR200
    NvDiagCapture( L"Ddr200CardInfo", SdhcExtension->T214CardInfoValid);
    NvDiagCapture( L"Ddr200Capable", (SdhcExtension->T214CardInfoValid & 3) == 3 &&
                   T214Ddr200Supported(SdhcExtension->T214Scr,
                                       SdhcExtension->T214SwitchStatus));
    NvDiagCapture( L"Ddr200Active", SdhcExtension->T214DdrActive);
    NvDiagCapture( L"Ddr200Status", SdhcExtension->T214DdrStatus);
    NvDiagCapture( L"Ddr200RecoveryStatus", SdhcExtension->T214DdrRecoveryStatus);
    NvDiagCapture( L"Ddr200Fallbacks", SdhcExtension->T214DdrFallbacks);
    NvDiagCapture( L"Ddr200Window", SdhcExtension->T214DdrWindow);
    NvDiagCapture( L"Ddr200Tap", SdhcExtension->T214DdrTap);
#endif
#endif
#if NV_T210_UHS
    NvDiagCapture( L"UhsFlags",
                   (SdhcExtension->T210Enabled ? 1u : 0u) |
                   (SdhcExtension->T210Sdr104 ? 2u : 0u));
    NvDiagCapture( L"UhsMuxValue", SdhcExtension->T210UhsMux);
    NvDiagCapture( L"UhsInitStatus", SdhcExtension->T210InitStatus);
    NvDiagCapture( L"UhsSignaling18V", SdhcExtension->T210Voltage18);
    NvDiagCapture( L"UhsVoltageStatus", SdhcExtension->T210VoltageStatus);
    NvDiagCapture( L"UhsVoltageFailures", SdhcExtension->T210VoltageFailures);
    NvDiagCapture( L"UhsTuneStatus", SdhcExtension->T210TuneStatus);
    NvDiagCapture( L"UhsTuned", SdhcExtension->T210Tuned);
    NvDiagCapture( L"UhsTap", SdhcExtension->T210Tap);
    NvDiagCapture( L"UhsKhz", SdhcExtension->T210BaseKhz);
    /* Callback trail + live registers: what the hardware is actually in right
       now, independent of what SDPORT believes it negotiated. */
    NvDiagCapture( L"UhsSigOn", SdhcExtension->T210SigOn);
    NvDiagCapture( L"UhsSigOff", SdhcExtension->T210SigOff);
    NvDiagCapture( L"UhsSigStatus", SdhcExtension->T210SigStatus);
    NvDiagCapture( L"UhsHold18V", SdhcExtension->T210Hold18V);
    NvDiagCapture( L"UhsOcrPatched", SdhcExtension->T210OcrPatched);
    NvDiagCapture( L"UhsOcrFirst", SdhcExtension->T210OcrFirst);
    NvDiagCapture( L"UhsOcrUhs", SdhcExtension->T210OcrUhs);
    NvDiagCapture( L"UhsOcrFirstN", SdhcExtension->T210OcrFirstCount);
    NvDiagCapture( L"UhsOcrUhsN", SdhcExtension->T210OcrUhsCount);
    NvDiagCapture( L"UhsSpeedSeen", SdhcExtension->T210SpeedSeen);
    NvDiagCapture( L"UhsCmd6R1", SdhcExtension->T210Cmd6R1);
    NvDiagCapture( L"UhsCmd7R1", SdhcExtension->T210Cmd7R1);
    NvDiagCapture( L"UhsCmd13R1", SdhcExtension->T210Cmd13R1);
    NvDiagCapture( L"UhsR1Count", SdhcExtension->T210R1Count);
    NvDiagCapture( L"UhsWinMs", SdhcExtension->T210WinMs);
    NvDiagCapture( L"UhsSnapPs", SdhcExtension->T210SnapPs);
    NvDiagCapture( L"UhsSnapHc2", SdhcExtension->T210SnapHc2);
    NvDiagCapture( L"UhsSnapPwr", SdhcExtension->T210SnapPwr);
    NvDiagCapture( L"UhsSnapClk", SdhcExtension->T210SnapClk);
    NvDiagCapture( L"UhsSnapIs", SdhcExtension->T210SnapIs);
    NvDiagCapture( L"UhsSnapErr", SdhcExtension->T210SnapErr);
    NvDiagCapture( L"UhsR1Idx0", SdhcExtension->T210R1Idx[0]);
    NvDiagCapture( L"UhsR1Idx1", SdhcExtension->T210R1Idx[1]);
    NvDiagCapture( L"UhsR1Idx2", SdhcExtension->T210R1Idx[2]);
    NvDiagCapture( L"UhsR1Idx3", SdhcExtension->T210R1Idx[3]);
    NvDiagCapture( L"UhsR1Idx4", SdhcExtension->T210R1Idx[4]);
    NvDiagCapture( L"UhsR1Idx5", SdhcExtension->T210R1Idx[5]);
    NvDiagCapture( L"UhsR1Idx6", SdhcExtension->T210R1Idx[6]);
    NvDiagCapture( L"UhsR1Idx7", SdhcExtension->T210R1Idx[7]);
    NvDiagCapture( L"UhsR1Val0", SdhcExtension->T210R1Val[0]);
    NvDiagCapture( L"UhsR1Val1", SdhcExtension->T210R1Val[1]);
    NvDiagCapture( L"UhsR1Val2", SdhcExtension->T210R1Val[2]);
    NvDiagCapture( L"UhsR1Val3", SdhcExtension->T210R1Val[3]);
    NvDiagCapture( L"UhsR1Val4", SdhcExtension->T210R1Val[4]);
    NvDiagCapture( L"UhsR1Val5", SdhcExtension->T210R1Val[5]);
    NvDiagCapture( L"UhsR1Val6", SdhcExtension->T210R1Val[6]);
    NvDiagCapture( L"UhsR1Val7", SdhcExtension->T210R1Val[7]);
    NvDiagCapture( L"UhsR1Idx8", SdhcExtension->T210R1Idx[8]);
    NvDiagCapture( L"UhsR1Idx9", SdhcExtension->T210R1Idx[9]);
    NvDiagCapture( L"UhsR1Idx10", SdhcExtension->T210R1Idx[10]);
    NvDiagCapture( L"UhsR1Idx11", SdhcExtension->T210R1Idx[11]);
    NvDiagCapture( L"UhsR1Idx12", SdhcExtension->T210R1Idx[12]);
    NvDiagCapture( L"UhsR1Idx13", SdhcExtension->T210R1Idx[13]);
    NvDiagCapture( L"UhsR1Idx14", SdhcExtension->T210R1Idx[14]);
    NvDiagCapture( L"UhsR1Idx15", SdhcExtension->T210R1Idx[15]);
    NvDiagCapture( L"UhsR1Val8", SdhcExtension->T210R1Val[8]);
    NvDiagCapture( L"UhsR1Val9", SdhcExtension->T210R1Val[9]);
    NvDiagCapture( L"UhsR1Val10", SdhcExtension->T210R1Val[10]);
    NvDiagCapture( L"UhsR1Val11", SdhcExtension->T210R1Val[11]);
    NvDiagCapture( L"UhsR1Val12", SdhcExtension->T210R1Val[12]);
    NvDiagCapture( L"UhsR1Val13", SdhcExtension->T210R1Val[13]);
    NvDiagCapture( L"UhsR1Val14", SdhcExtension->T210R1Val[14]);
    NvDiagCapture( L"UhsR1Val15", SdhcExtension->T210R1Val[15]);
    NvDiagCapture( L"UhsScr0", SdhcExtension->T210Scr[0]);
    NvDiagCapture( L"UhsScr1", SdhcExtension->T210Scr[1]);
    NvDiagCapture( L"UhsFn0", SdhcExtension->T210FnTable[0]);
    NvDiagCapture( L"UhsFn1", SdhcExtension->T210FnTable[1]);
    NvDiagCapture( L"UhsFn2", SdhcExtension->T210FnTable[2]);
    NvDiagCapture( L"UhsFn3", SdhcExtension->T210FnTable[3]);
    NvDiagCapture( L"UhsTraceCount", SdhcExtension->T210TraceCount);
    NvDiagCapture( L"UhsCmdCount", SdhcExtension->T210CmdCount);
    NvDiagCapture( L"UhsCmd00", ((ULONG)SdhcExtension->T210CmdRing[3] << 24) | ((ULONG)SdhcExtension->T210CmdRing[2] << 16) | ((ULONG)SdhcExtension->T210CmdRing[1] << 8) | SdhcExtension->T210CmdRing[0]);
    NvDiagCapture( L"UhsCmd01", ((ULONG)SdhcExtension->T210CmdRing[7] << 24) | ((ULONG)SdhcExtension->T210CmdRing[6] << 16) | ((ULONG)SdhcExtension->T210CmdRing[5] << 8) | SdhcExtension->T210CmdRing[4]);
    NvDiagCapture( L"UhsCmd02", ((ULONG)SdhcExtension->T210CmdRing[11] << 24) | ((ULONG)SdhcExtension->T210CmdRing[10] << 16) | ((ULONG)SdhcExtension->T210CmdRing[9] << 8) | SdhcExtension->T210CmdRing[8]);
    NvDiagCapture( L"UhsCmd03", ((ULONG)SdhcExtension->T210CmdRing[15] << 24) | ((ULONG)SdhcExtension->T210CmdRing[14] << 16) | ((ULONG)SdhcExtension->T210CmdRing[13] << 8) | SdhcExtension->T210CmdRing[12]);
    NvDiagCapture( L"UhsCmd04", ((ULONG)SdhcExtension->T210CmdRing[19] << 24) | ((ULONG)SdhcExtension->T210CmdRing[18] << 16) | ((ULONG)SdhcExtension->T210CmdRing[17] << 8) | SdhcExtension->T210CmdRing[16]);
    NvDiagCapture( L"UhsCmd05", ((ULONG)SdhcExtension->T210CmdRing[23] << 24) | ((ULONG)SdhcExtension->T210CmdRing[22] << 16) | ((ULONG)SdhcExtension->T210CmdRing[21] << 8) | SdhcExtension->T210CmdRing[20]);
    NvDiagCapture( L"UhsCmd06", ((ULONG)SdhcExtension->T210CmdRing[27] << 24) | ((ULONG)SdhcExtension->T210CmdRing[26] << 16) | ((ULONG)SdhcExtension->T210CmdRing[25] << 8) | SdhcExtension->T210CmdRing[24]);
    NvDiagCapture( L"UhsCmd07", ((ULONG)SdhcExtension->T210CmdRing[31] << 24) | ((ULONG)SdhcExtension->T210CmdRing[30] << 16) | ((ULONG)SdhcExtension->T210CmdRing[29] << 8) | SdhcExtension->T210CmdRing[28]);
    NvDiagCapture( L"UhsTrace0", SdhcExtension->T210Trace[0]);
    NvDiagCapture( L"UhsTrace1", SdhcExtension->T210Trace[1]);
    NvDiagCapture( L"UhsTrace2", SdhcExtension->T210Trace[2]);
    NvDiagCapture( L"UhsTrace3", SdhcExtension->T210Trace[3]);
    NvDiagCapture( L"UhsTrace4", SdhcExtension->T210Trace[4]);
    NvDiagCapture( L"UhsTrace5", SdhcExtension->T210Trace[5]);
    NvDiagCapture( L"UhsTrace6", SdhcExtension->T210Trace[6]);
    NvDiagCapture( L"UhsTrace7", SdhcExtension->T210Trace[7]);
    NvDiagCapture( L"LiveHc2", SdhcReadRegisterUshort(SdhcExtension, SDHC_HOST_CONTROL2));
    NvDiagCapture( L"LivePwr", SdhcReadRegisterUchar(SdhcExtension, SDHC_POWER_CONTROL));
    NvDiagCapture( L"LiveClk", SdhcReadRegisterUshort(SdhcExtension, SDHC_CLOCK_CONTROL));
    NvDiagCapture( L"LivePs", SdhcReadRegisterUlong(SdhcExtension, SDHC_PRESENT_STATE));
    NvDiagCapture( L"LiveHc1", SdhcReadRegisterUchar(SdhcExtension, SDHC_HOST_CONTROL));
#if NV_T210_UHS_DMA
    NvDiagCapture( L"UhsDmaRequests", SdhcExtension->T210DmaRequests);
    NvDiagCapture( L"UhsDmaBlocks", SdhcExtension->T210DmaBlocks);
    NvDiagCapture( L"UhsDmaErrors", SdhcExtension->T210DmaErrors);
    NvDiagCapture( L"UhsDmaErrStatus", SdhcExtension->T210DmaLastError);
    NvDiagCapture( L"UhsDmaAdmaErr", SdhcExtension->T210DmaLastAdmaError);
    NvDiagCapture( L"UhsDmaMaxBlocks", SdhcExtension->T210DmaMaxBlocks);
    NvDiagCapture( L"UhsPioFallbacks", SdhcExtension->T210PioFallbacks);
    NvDiagCapture( L"UhsDmaBytes", SdhcExtension->T210DmaCapacity);
    NvDiagCapture( L"UhsDmaTest", SdhcExtension->T210DmaTestResult);
    NvDiagCapture( L"UhsDmaStDma", SdhcExtension->T210DmaTestStDma);
    NvDiagCapture( L"UhsDmaStPio", SdhcExtension->T210DmaTestStPio);
    NvDiagCapture( L"UhsDmaIs", SdhcExtension->T210DmaTestIs);
    NvDiagCapture( L"UhsDmaErr", SdhcExtension->T210DmaTestErr);
    NvDiagCapture( L"UhsDmaAdma", SdhcExtension->T210DmaTestAdma);
    NvDiagCapture( L"UhsDmaPs", SdhcExtension->T210DmaTestPs);
    NvDiagCapture( L"UhsSmmu2", SdhcExtension->T210Smmu2);
    NvDiagCapture( L"UhsDmaOk", SdhcExtension->T210DmaOk ? 1u : 0u);
    NvDiagCapture( L"UhsSmmu", SdhcExtension->T210Smmu);
    NvDiagCapture( L"UhsSmmu2", SdhcExtension->T210Smmu2);
    NvDiagCapture( L"UhsSmmu3", SdhcExtension->T210Smmu3);
    NvDiagCapture( L"UhsSmmu4", SdhcExtension->T210Smmu4);
#endif
#endif
#if NV_T210_LITE_WINARM_WRITE
    NvDiagCapture( L"App42Done", NvDiag.App42Done);
    NvDiagCapture( L"DeniedCount", NvDiag.DeniedCount);
    NvDiagCapture( L"LastDenied", NvDiag.LastDeniedCommand);
    NvDiagCapture( L"DeniedArg", NvDiag.LastDeniedArgument);
    NvDiagCapture( L"DeniedShape", NvDiag.DeniedShape);
    NvDiagCapture( L"TargetFacts", (ULONG)SdhcExtension->TargetFacts);
#endif
    NvDiagCapture( L"LastCommand", NvDiag.LastCommand);
    NvDiagCapture( L"ClockKhzRequested", NvDiag.ClockKhzRequested);
    NvDiagCapture( L"ClockKhzActual", NvDiag.ClockKhzActual);
    NvDiagCapture( L"BaseClockKhz",
                   SdhcExtension->Capabilities.BaseClockFrequencyKhz);
    NvDiagCapture( L"RegCapabilities", NvDiag.RegCapabilities);
    NvDiagCapture( L"RegClockControl", NvDiag.RegClockControl);
    NvDiagCapture( L"RegHostControl", NvDiag.RegHostControl);
    NvDiagCapture( L"BusWidth", NvDiag.BusWidth);
    NvDiagCapture( L"BusSpeed", NvDiag.BusSpeed);
    NvDiagCapture( L"Commands", NvDiag.Commands);
    NvDiagCapture( L"BlocksRead", NvDiag.BlocksRead);
    NvDiagCapture( L"CarSdmmc1Before", NvDiag.CarSdmmc1Before);
    NvDiagCapture( L"CarSdmmc1After", NvDiag.CarSdmmc1After);
    NvDiagCapture( L"BlocksWritten", NvDiag.BlocksWritten);
    NvDiagCapture( L"Interrupts", NvDiag.Interrupts);
    NvDiagCapture( L"ErrorInterrupts", NvDiag.ErrorInterrupts);
    NvDiagCapture( L"CommandErrors", NvDiag.CommandErrors);
    NvDiagCapture( L"LastErrors", NvDiag.LastErrors);
    NvDiagCapture( L"LastErrorCommand", NvDiag.LastErrorCommand);
    NvDiagCapture( L"DataCommands", NvDiag.DataCommands);
    NvDiagCapture( L"DataBlocks", NvDiag.DataBlocks);
    NvDiagCapture( L"DataStartBlocks", NvDiag.DataStartBlocks);
    NvDiagCapture( L"DataUs", NvDiag.DataUs);
    NvDiagCapture( L"GapUs", NvDiag.GapUs);
    NvDiagCapture( L"SgDmaRequests", NvDiag.SgDmaRequests);
    NvDiagCapture( L"SgDmaBlocks", NvDiag.SgDmaBlocks);
    NvDiagCapture( L"SgDmaErrors", NvDiag.SgDmaErrors);
    NvDiagCapture( L"SgDmaErrStatus", NvDiag.SgDmaErrStatus);
    NvDiagCapture( L"SgDmaAdmaErr", NvDiag.SgDmaAdmaErr);
    NvDiagCapture( L"PioCalls", NvDiag.PioCalls);
    NvDiagCapture( L"PioCallBlocks", NvDiag.PioCallBlocks);
    NvDiagCapture( L"PioMore", NvDiag.PioCallMore);
    NvDiagCapture( L"PioReadyMiss", NvDiag.PioReadyMiss);
    NvDiagCapture( L"PioBudgetHit", NvDiag.PioBudgetHit);
    NvDiagCapture( L"PioPollIters", NvDiag.PioPollIters);
    NvDiagCapture( L"PioPollUs", NvPioUs(NvDiag.PioPollTicks));
    NvDiagCapture( L"PioPortUs", NvPioUs(NvDiag.PioPortTicks));
    NvDiagCapture( L"PioCallUs", NvPioUs(NvDiag.PioCallTicks));
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    ExInitializeWorkItem(&NvDiagSaveItem, NvDiagSaveWorker, SdhcExtension);
    ExQueueWorkItem(&NvDiagSaveItem, DelayedWorkQueue);
#pragma clang diagnostic pop
}

static VOID
NvDiagSavePeriodic(
    _In_ PSDHC_EXTENSION SdhcExtension
    )
{
    if ((LONG64)KeQueryInterruptTime() >=
        InterlockedCompareExchange64(&NvDiagNextPeriodicSave, 0, 0)) {
        NvDiagSaveClock(SdhcExtension);
    }
}

/* Fires once, NV_DIAG_LATE_SECONDS after the slot came up. It only flips the
 * gate and queues one snapshot; the MMIO probe and the registry write happen
 * in the PASSIVE work item, never in this DPC. */
static VOID
NvDiagLateRoutine(
    _In_ PKDPC Dpc,
    _In_opt_ PVOID Context,
    _In_opt_ PVOID Arg1,
    _In_opt_ PVOID Arg2
    )
{
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Arg1);
    UNREFERENCED_PARAMETER(Arg2);
    InterlockedExchange(&NvDiagLate, 1);
    if (Context != NULL) NvDiagSaveClock((PSDHC_EXTENSION)Context);
}

static VOID
NvDiagScheduleLate(
    _In_ PSDHC_EXTENSION SdhcExtension
    )
{
    LARGE_INTEGER Due;

    KeInitializeTimer(&NvDiagLateTimer);
    KeInitializeDpc(&NvDiagLateDpc, NvDiagLateRoutine, SdhcExtension);
    Due.QuadPart = -10000000LL * NV_DIAG_LATE_SECONDS;
    KeSetTimer(&NvDiagLateTimer, Due, &NvDiagLateDpc);
}
#endif

#if NV_T210_UHS && NV_T210_UHS_DEFERRED
/* ---- T210 deferred UHS probe/activation ---------------------------------
 * The boot path never touches UHS: SdhcSlotInitialize leaves the slot on the
 * verified 3.3 V high-speed path and nothing is advertised to SDPORT. This
 * worker runs once, well after boot, and walks the UHS sequence in stages
 * (NV_T210_UHS_STAGE). Every step records a flushed registry marker, so a
 * crash or a hang still leaves the reached stage behind. Failures use soft
 * restore (T210SoftMode): the card is the live boot disk and must never be
 * power-cycled from here. */
#define NV_T210_LATE_SECONDS 300
static WORK_QUEUE_ITEM NvT210Item;
static volatile LONG NvT210Queued;
static volatile LONG NvT210Started;
static KTIMER NvT210Timer;
static KDPC NvT210TimerDpc;

#if NV_DIAG_CLOCK
#define NvT210Diag(Name, Value) NvDiagCapture(Name, Value)
#else
#define NvT210Diag(Name, Value) ((void)0)
#endif

/* Flushed marker write. Only ever called from the PASSIVE worker, never from
   an SD port callback (registry I/O there re-enters the boot miniport). */
static VOID
NvT210Mark(
    _In_ PCWSTR Name,
    _In_ ULONG Value
    )
{
    UNICODE_STRING Path;
    OBJECT_ATTRIBUTES Attributes;
    HANDLE Key = NULL;

    if (NvDiag.RegistryPath[0] == L'\0') return;
    RtlInitUnicodeString(&Path, NvDiag.RegistryPath);
    InitializeObjectAttributes(&Attributes, &Path,
                               OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (!NT_SUCCESS(ZwCreateKey(&Key, KEY_SET_VALUE, &Attributes, 0, NULL,
                                REG_OPTION_NON_VOLATILE, NULL))) {
        return;
    }
    NvDiagSetValue(Key, Name, Value);
    ZwFlushKey(Key);
    ZwClose(Key);
}

static BOOLEAN
NvT210BusIdle(
    _In_ PSDHC_EXTENSION s
    )
{
    if (s->OutstandingRequests[0] != NULL) return FALSE;
    return (SdhcReadRegisterUlong(s, SDHC_PRESENT_STATE) &
            (SDHC_PS_CMD_INHIBIT | SDHC_PS_DAT_INHIBIT)) == 0;
}

/* Catch a genuinely quiet bus. This card is the boot volume and SDPORT serves
 * it continuously, so one short window is not enough (the stage0 run gave up
 * after a 600 ms window, UhsBusy = 14). Rounds of 10 ms of sampling are spaced
 * by one second, for about 30 s in total. A window only counts once two idle
 * samples 1 ms apart agree, so a command that starts between two reads is not
 * mistaken for an idle bus. */
static BOOLEAN
NvT210WaitIdle(
    _In_ PSDHC_EXTENSION s,
    _Out_ PULONG Tries
    )
{
    LARGE_INTEGER Delay;
    ULONG Round, Spin;

    *Tries = 0;
    for (Round = 0; Round < 30; ++Round) {
        for (Spin = 0; Spin < 40; ++Spin, ++*Tries) {
            if (NvT210BusIdle(s)) {
                KeStallExecutionProcessor(1000);
                if (NvT210BusIdle(s)) return TRUE;
            }
            KeStallExecutionProcessor(250);
        }
        Delay.QuadPart = -10000000LL;
        KeDelayExecutionThread(KernelMode, FALSE, &Delay);
    }
    return FALSE;
}

#if NV_T210_UHS_STAGE >= 1
static const PCWSTR NvT210FnNames[16] = {
    L"UhsFn00", L"UhsFn01", L"UhsFn02", L"UhsFn03",
    L"UhsFn04", L"UhsFn05", L"UhsFn06", L"UhsFn07",
    L"UhsFn08", L"UhsFn09", L"UhsFn10", L"UhsFn11",
    L"UhsFn12", L"UhsFn13", L"UhsFn14", L"UhsFn15"
};
static const PCWSTR NvT210FnNames2[16] = {
    L"UhsFnB00", L"UhsFnB01", L"UhsFnB02", L"UhsFnB03",
    L"UhsFnB04", L"UhsFnB05", L"UhsFnB06", L"UhsFnB07",
    L"UhsFnB08", L"UhsFnB09", L"UhsFnB10", L"UhsFnB11",
    L"UhsFnB12", L"UhsFnB13", L"UhsFnB14", L"UhsFnB15"
};
static const PCWSTR NvT210SelNames[16] = {
    L"UhsSel00", L"UhsSel01", L"UhsSel02", L"UhsSel03",
    L"UhsSel04", L"UhsSel05", L"UhsSel06", L"UhsSel07",
    L"UhsSel08", L"UhsSel09", L"UhsSel10", L"UhsSel11",
    L"UhsSel12", L"UhsSel13", L"UhsSel14", L"UhsSel15"
};

/* MAX77620 read-only dump (register map from NintendoSwitchPkg Max77620.h):
   the rails UEFI's SdPowerOn() touches plus every SD/LDO config register. */
typedef struct _NV_T210_PMIC_REG { UCHAR Register; PCWSTR Name; } NV_T210_PMIC_REG;
static const NV_T210_PMIC_REG NvT210PmicRegs[] = {
    { 0x00, L"UhsPmicGlbl1" }, { 0x02, L"UhsPmicGlbl3" }, { 0x14, L"UhsPmicStatSd" },
    { 0x16, L"UhsPmicSd0" },   { 0x17, L"UhsPmicSd1" },   { 0x18, L"UhsPmicSd2" },
    { 0x19, L"UhsPmicSd3" },   { 0x1A, L"UhsPmicSd4" },   { 0x1B, L"UhsPmicDvsSd0" },
    { 0x1C, L"UhsPmicDvsSd1" }, { 0x1D, L"UhsPmicSd0Cfg" }, { 0x1E, L"UhsPmicSd1Cfg" },
    { 0x1F, L"UhsPmicSd2Cfg" }, { 0x20, L"UhsPmicSd3Cfg" }, { 0x21, L"UhsPmicSd4Cfg" },
    { 0x22, L"UhsPmicSdCfg2" }, { 0x23, L"UhsPmicLdo0Cfg" }, { 0x24, L"UhsPmicLdo0Cfg2" },
    { 0x25, L"UhsPmicLdo1Cfg" }, { 0x26, L"UhsPmicLdo1Cfg2" }, { 0x27, L"UhsPmicLdo2Cfg" },
    { 0x28, L"UhsPmicLdo2Cfg2" }, { 0x29, L"UhsPmicLdo3Cfg" }, { 0x2A, L"UhsPmicLdo3Cfg2" },
    { 0x2B, L"UhsPmicLdo4Cfg" }, { 0x2C, L"UhsPmicLdo4Cfg2" }, { 0x2D, L"UhsPmicLdo5Cfg" },
    { 0x2F, L"UhsPmicLdo6Cfg" }, { 0x31, L"UhsPmicLdo7Cfg" }, { 0x33, L"UhsPmicLdo8Cfg" },
    { 0x35, L"UhsPmicLdoCfg3" }
};
#define NV_T210_PMIC_REGS (ULONG)(sizeof(NvT210PmicRegs) / sizeof(NvT210PmicRegs[0]))
#endif

static VOID
NvT210Worker(
    _In_ PSDHC_EXTENSION s
    )
{
    NTSTATUS st;
    ULONG IdleTries = 0;
    LARGE_INTEGER Delay;
    ULONG i;

    if (s == NULL) return;

    NvT210Mark(L"UhsStage", NV_T210_UHS_STAGE);
    NvT210Mark(L"UhsLateSec", NV_T210_LATE_SECONDS);

    /* The boot volume can stay busy for a while after the delay above (one
       run saw 30 s of continuous traffic), so the quiet window is retried for
       up to ~8 minutes before giving up for this boot. */
    for (i = 0; i < 8; ++i) {
        if (NvT210WaitIdle(s, &IdleTries)) break;
        NvT210Mark(L"UhsBusyTry", i + 1);
        Delay.QuadPart = -300000000LL;                    /* 30 s */
        KeDelayExecutionThread(KernelMode, FALSE, &Delay);
    }
    NvT210Mark(L"UhsBusy", IdleTries);
    if (i >= 8) {
        NvT210Mark(L"UhsState", 3);
        return;
    }

    /* Map the platform registers (no writes yet). */
    s->T210UhsMux = 3;
    st = T210UhsInit(s);
    NvT210Mark(L"UhsInit", (ULONG)st);
    NvT210Mark(L"UhsReady", s->T210Ready ? 1u : 0u);
    if (!NT_SUCCESS(st)) {
        NvT210Mark(L"UhsState", 4);
        return;
    }

#if NV_T210_UHS_STAGE >= 1
    /* Read-only probe of the host state and the LDO2 rail. */
    NvT210Mark(L"UhsStPresent", SdhcReadRegisterUlong(s, SDHC_PRESENT_STATE));
    NvT210Mark(L"UhsStClock", SdhcReadRegisterUshort(s, SDHC_CLOCK_CONTROL));
    NvT210Mark(L"UhsStHc2", SdhcReadRegisterUshort(s, SDHC_HOST_CONTROL2));
    if (s->T210Ready) {
        NTSTATUS g = TegraI2c5Acquire(s->T210Gate);
        NvT210Mark(L"UhsStGate", (ULONG)g);
        if (NT_SUCCESS(g)) {
            ULONG v = 0x27, a = 0x27;
            NTSTATUS r1 = T210PmicMessageAt(s->T210I2c, FALSE, &v, 1);
            NTSTATUS r2 = NT_SUCCESS(r1) ? T210PmicMessageAt(s->T210I2c, TRUE, &v, 1) : r1;
            NTSTATUS r3 = T210PmicMessageAt(s->T210I2cAlt, FALSE, &a, 1);
            NTSTATUS r4 = NT_SUCCESS(r3) ? T210PmicMessageAt(s->T210I2cAlt, TRUE, &a, 1) : r3;
            NvT210Mark(L"UhsStLdo2", NT_SUCCESS(r2) ? (v & 0xff) : 0xFFFFFFFF);
            NvT210Mark(L"UhsStLdo2Alt", NT_SUCCESS(r4) ? (a & 0xff) : 0xFFFFFFFF);

            /* Read-only PMIC dump: which rail really feeds the card's IO, and
               whether LDO2 (0x27, what UEFI programs to 3.3 V) is still on.
               MAX77620 map: SD0..SD4 = 0x16..0x1A, SD*_CFG = 0x1D..0x21,
               LDO0..8_CFG = 0x23/0x25/0x27/0x29/0x2B/0x2D/0x2F/0x31/0x33. */
            for (i = 0; i < NV_T210_PMIC_REGS; ++i) {
                ULONG reg = NvT210PmicRegs[i].Register;
                ULONG val = reg;
                NTSTATUS rs = T210PmicMessageAt(s->T210I2c, FALSE, &val, 1);
                if (NT_SUCCESS(rs)) rs = T210PmicMessageAt(s->T210I2c, TRUE, &val, 1);
                NvT210Mark(NvT210PmicRegs[i].Name,
                           NT_SUCCESS(rs) ? (val & 0xff) : 0xFFFFFFFF);
            }
            TegraI2c5Release(s->T210Gate);
        } else {
            NvT210Mark(L"UhsStLdo2", 0xFFFFFFFE);
            NvT210Mark(L"UhsStLdo2Alt", 0xFFFFFFFE);
        }
    }

    /* CMD6 SWITCH_FUNC in check-function mode: the card's own statement of
       which bus modes it supports. Nothing on the host or card changes; the
       64-byte status block is dumped word by word for offline decoding. */
    {
        T210_SWITCH_STATUS Fn, Fn2;
        NTSTATUS FnStatus = STATUS_RETRY, FnStatus2 = STATUS_RETRY;
        ULONG Used = 0, t = 0, Diff = 0, identical = 1;

        RtlZeroMemory(&Fn, sizeof(Fn));
        RtlZeroMemory(&Fn2, sizeof(Fn2));
        for (i = 0; i < 3; ++i) {
            NvT210Mark(L"UhsFnAttempt", i + 1);
            if (!NvT210WaitIdle(s, &t)) {
                NvT210Mark(L"UhsFnIdle", t);
                continue;
            }
            FnStatus = T210Cmd6(s, T210_CMD6_ARG_QUERY, &Fn);
            Used = i + 1;
            if (NT_SUCCESS(FnStatus)) break;
            Delay.QuadPart = -20000000LL;             /* 2 s before a retry */
            KeDelayExecutionThread(KernelMode, FALSE, &Delay);
        }

        /* Ask twice: the block must be repeatable, otherwise the data phase
           is not trustworthy and the decode below means nothing. */
        if (NT_SUCCESS(FnStatus) && NvT210WaitIdle(s, &t)) {
            FnStatus2 = T210Cmd6(s, T210_CMD6_ARG_QUERY, &Fn2);
            for (i = 0; i < 16; ++i) {
                if (Fn.Word[i] != Fn2.Word[i]) {
                    if (identical) Diff = i;
                    identical = 0;
                }
            }
        }

        NvT210Mark(L"UhsFnTry", Used);
        NvT210Mark(L"UhsFnSt", (ULONG)FnStatus);
        NvT210Mark(L"UhsFnSt2", (ULONG)FnStatus2);
        NvT210Mark(L"UhsFnRepeat", identical ? 1u : 0u);
        NvT210Mark(L"UhsFnDiff", Diff);
        NvT210Mark(L"UhsFnErr", Fn.ErrorStatus);
        NvT210Mark(L"UhsFnIs", Fn.InterruptStatus);
        NvT210Mark(L"UhsFnPs", Fn.PresentState);
        NvT210Mark(L"UhsFnR1", Fn.R1);
        NvT210Mark(L"UhsFnModes", T210SwitchGroup1(&Fn));
        for (i = 0; i < 16; ++i) {
            NvT210Mark(NvT210FnNames[i], Fn.Word[i]);
            NvT210Mark(NvT210FnNames2[i], Fn2.Word[i]);
        }

        /* CMD8 (SEND_IF_COND) and CMD58 (READ_OCR) were tried here first and
           both timed out (CMD_TIMEOUT, error status 0x1) although CMD6 worked;
           they are only answered during card identification, so a timeout in
           tran state says nothing about the card.

           What follows asks the card directly: select High Speed (the mode the
           bus already runs in), then SDR104, then High Speed again. R1 bit 7
           (SWITCH_ERROR) is the card's own verdict on each request, and the
           status block is re-read after each step. */
        {
            T210_SWITCH_STATUS Sel, After;
            ULONG t3 = 0;

            RtlZeroMemory(&Sel, sizeof(Sel));
            RtlZeroMemory(&After, sizeof(After));
            if (NvT210WaitIdle(s, &t3)) {
                (VOID)T210Cmd6Select(s, 1, &Sel);          /* 1 = SDR25/HS */
            }
            NvT210Mark(L"UhsSelSt", (ULONG)Sel.Status);
            NvT210Mark(L"UhsSelR1", Sel.R1);
            NvT210Mark(L"UhsSelSwtErr", (Sel.R1 >> 7) & 1u);
            NvT210Mark(L"UhsSelErr", Sel.ErrorStatus);
            NvT210Mark(L"UhsSelIs", Sel.InterruptStatus);

            if (NvT210WaitIdle(s, &t3)) {
                NvT210Mark(L"UhsSelQSt", (ULONG)T210Cmd6(s, T210_CMD6_ARG_QUERY, &After));
            } else {
                NvT210Mark(L"UhsSelQSt", 0xE0000001);
            }
            NvT210Mark(L"UhsSelByte16", (After.Word[4] & 0xffu));
            NvT210Mark(L"UhsSelModes", T210SwitchGroup1(&After));
            for (i = 0; i < 16; ++i) {
                NvT210Mark(NvT210SelNames[i], After.Word[i]);
            }

            /* The decisive question: does the card accept SDR104 at all? */
            RtlZeroMemory(&Sel, sizeof(Sel));
            RtlZeroMemory(&After, sizeof(After));
            if (NvT210WaitIdle(s, &t3)) {
                (VOID)T210Cmd6Select(s, 3, &Sel);          /* 3 = SDR104 */
            }
            NvT210Mark(L"UhsS104St", (ULONG)Sel.Status);
            NvT210Mark(L"UhsS104R1", Sel.R1);
            NvT210Mark(L"UhsS104SwtErr", (Sel.R1 >> 7) & 1u);
            NvT210Mark(L"UhsS104Err", Sel.ErrorStatus);
            if (NvT210WaitIdle(s, &t3)) {
                NvT210Mark(L"UhsS104QSt", (ULONG)T210Cmd6(s, T210_CMD6_ARG_QUERY, &After));
            } else {
                NvT210Mark(L"UhsS104QSt", 0xE0000001);
            }
            NvT210Mark(L"UhsS104Byte16", (After.Word[4] & 0xffu));
            NvT210Mark(L"UhsS104Modes", T210SwitchGroup1(&After));

            /* Put the card back where the boot path left it, whatever the
               answer above was, and confirm with one more query. */
            RtlZeroMemory(&Sel, sizeof(Sel));
            RtlZeroMemory(&After, sizeof(After));
            if (NvT210WaitIdle(s, &t3)) {
                (VOID)T210Cmd6Select(s, 1, &Sel);
            }
            NvT210Mark(L"UhsRevertSt", (ULONG)Sel.Status);
            NvT210Mark(L"UhsRevertR1", Sel.R1);
            NvT210Mark(L"UhsRevertSwtErr", (Sel.R1 >> 7) & 1u);
            if (NvT210WaitIdle(s, &t3)) {
                NvT210Mark(L"UhsRevertQSt", (ULONG)T210Cmd6(s, T210_CMD6_ARG_QUERY, &After));
            } else {
                NvT210Mark(L"UhsRevertQSt", 0xE0000001);
            }
            NvT210Mark(L"UhsRevertByte16", (After.Word[4] & 0xffu));
            NvT210Mark(L"UhsRevertModes", T210SwitchGroup1(&After));
        }

        /* Liveness check: after the switch above CMD6 must still work. */
        {
            T210_SWITCH_STATUS Fn3;
            ULONG t4 = 0;

            RtlZeroMemory(&Fn3, sizeof(Fn3));
            if (NvT210WaitIdle(s, &t4)) {
                NvT210Mark(L"UhsFnSt3", (ULONG)T210Cmd6(s, T210_CMD6_ARG_QUERY, &Fn3));
            } else {
                NvT210Mark(L"UhsFnSt3", 0xE0000001);
            }
        }
    }
#endif

#if NV_T210_UHS_STAGE >= 2
    /* 1.8 V signalling switch, soft restore (card stays powered). */
    T210SoftMode = TRUE;
    st = T210Signaling(s, TRUE);
    T210SoftMode = FALSE;
    NvT210Mark(L"UhsSignal", (ULONG)st);
    NvT210Mark(L"Uhs18V", s->T210Voltage18 ? 1u : 0u);
    NvT210Mark(L"UhsVStatus", s->T210VoltageStatus);
    if (!NT_SUCCESS(st)) {
        NvT210Mark(L"UhsState", 5);
        return;
    }
#endif

#if NV_T210_UHS_STAGE >= 3
    {
        USHORT hc, cc;
        hc = SdhcReadRegisterUshort(s, SDHC_HOST_CONTROL2);
        hc = (USHORT)((hc & ~SDHC_HC2_UHS_MODES) | SDHC_HC2_SDR104);
        SdhcWriteRegisterUshort(s, SDHC_HOST_CONTROL2, hc);
        st = T210ClockSdr104(s);
        NvT210Mark(L"UhsClock", (ULONG)st);
        cc = SdhcReadRegisterUshort(s, SDHC_CLOCK_CONTROL);
        cc = (USHORT)((cc & ~0xff00u) | SDHC_CC_CLOCK_DIVISOR_2);
        SdhcWriteRegisterUshort(s, SDHC_CLOCK_CONTROL, cc);
        KeStallExecutionProcessor(100);
        if (!NT_SUCCESS(st)) {
            NvT210Mark(L"UhsState", 6);
            return;
        }
    }
#endif

#if NV_T210_UHS_STAGE >= 4
    st = T210Tune(s);
    NvT210Mark(L"UhsTune", (ULONG)st);
    NvT210Mark(L"UhsTuned", s->T210Tuned ? 1u : 0u);
    NvT210Mark(L"UhsTap", s->T210Tap);
    NvT210Mark(L"UhsKhz", s->T210BaseKhz);
#endif

    NvT210Mark(L"UhsState", 1);
}

/* Runs on a system worker thread at PASSIVE: creating the dedicated thread is
 * the only thing done there, because the sequence below waits for minutes and
 * must not hold a shared worker. */
static VOID NvT210Thread(_In_ PVOID Context);

static VOID
NvT210Starter(
    _In_ PVOID Context
    )
{
    HANDLE Thread = NULL;

    if (Context == NULL) return;
    if (NT_SUCCESS(PsCreateSystemThread(&Thread, THREAD_ALL_ACCESS, NULL, NULL, NULL,
                                        NvT210Thread, Context)) && (Thread != NULL)) {
        ZwClose(Thread);
    }
}

static VOID
NvT210Thread(
    _In_ PVOID Context
    )
{
    if (InterlockedCompareExchange(&NvT210Started, 1, 0) == 0) {
        /* The timer that queued us already waited NV_T210_LATE_SECONDS, so the
           probe starts right away instead of waiting twice. */
        NvT210Worker((PSDHC_EXTENSION)Context);
        NvT210Started = 0;
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static VOID
NvT210LateRoutine(
    _In_ PKDPC Dpc,
    _In_opt_ PVOID Context,
    _In_opt_ PVOID Arg1,
    _In_opt_ PVOID Arg2
    )
{
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Arg1);
    UNREFERENCED_PARAMETER(Arg2);
    if (InterlockedCompareExchange(&NvT210Queued, 1, 0) != 0) return;
    ExInitializeWorkItem(&NvT210Item, NvT210Starter, Context);
    ExQueueWorkItem(&NvT210Item, DelayedWorkQueue);
}

static VOID
NvT210ScheduleLate(
    _In_ PSDHC_EXTENSION SdhcExtension
    )
{
    LARGE_INTEGER Due;
    KeInitializeTimer(&NvT210Timer);
    KeInitializeDpc(&NvT210TimerDpc, NvT210LateRoutine, SdhcExtension);
    Due.QuadPart = -10000000LL * NV_T210_LATE_SECONDS;
    KeSetTimer(&NvT210Timer, Due, &NvT210TimerDpc);
}
#endif

#if NV_DIAG_REG

static VOID
NvDiagThread(
    _In_ PVOID Context
    )
{
    UNICODE_STRING Path;
    OBJECT_ATTRIBUTES Attributes;
    HANDLE Key;
    LARGE_INTEGER Interval;
    PSDHC_EXTENSION Ext;
    ULONG Seconds = 0;

    UNREFERENCED_PARAMETER(Context);

    RtlInitUnicodeString(&Path, NvDiag.RegistryPath);
    InitializeObjectAttributes(&Attributes, &Path,
                               OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);

    Interval.QuadPart = -10000000LL * NV_DIAG_REG_SECONDS;
    for (;;) {
        KeDelayExecutionThread(KernelMode, FALSE, &Interval);
        Seconds += NV_DIAG_REG_SECONDS;
        if (!NT_SUCCESS(ZwCreateKey(&Key, KEY_SET_VALUE, &Attributes, 0, NULL,
                                    REG_OPTION_NON_VOLATILE, NULL))) {
            continue;
        }

        Ext = (PSDHC_EXTENSION) NvDiag.Extension;
        NvDiagSetValue(Key, L"Seconds", Seconds);
        NvDiagSetValue(Key, L"ClockKhzRequested", NvDiag.ClockKhzRequested);
        NvDiagSetValue(Key, L"ClockKhzActual", NvDiag.ClockKhzActual);
        NvDiagSetValue(Key, L"BusWidth", NvDiag.BusWidth);
        NvDiagSetValue(Key, L"BusSpeed", NvDiag.BusSpeed);
        NvDiagSetValue(Key, L"Interrupts", NvDiag.Interrupts);
        NvDiagSetValue(Key, L"ErrorInterrupts", NvDiag.ErrorInterrupts);
        NvDiagSetValue(Key, L"Commands", NvDiag.Commands);
        NvDiagSetValue(Key, L"CommandsOk", NvDiag.CommandsOk);
        NvDiagSetValue(Key, L"CommandErrors", NvDiag.CommandErrors);
        NvDiagSetValue(Key, L"LastErrors", NvDiag.LastErrors);
        NvDiagSetValue(Key, L"LastErrorCommand", NvDiag.LastErrorCommand);
        NvDiagSetValue(Key, L"BlocksRead", NvDiag.BlocksRead);
        NvDiagSetValue(Key, L"BlocksWritten", NvDiag.BlocksWritten);
        NvDiagSetValue(Key, L"RegCapabilities", NvDiag.RegCapabilities);
        NvDiagSetValue(Key, L"RegCapabilities2", NvDiag.RegCapabilities2);
        NvDiagSetValue(Key, L"RegClockControl", NvDiag.RegClockControl);
        NvDiagSetValue(Key, L"RegHostControl", NvDiag.RegHostControl);
        NvDiagSetValue(Key, L"RegPresentState", NvDiag.PresentState);
        if (Ext != NULL) {
            NvDiagSetValue(Key, L"BaseClockKhz",
                           Ext->Capabilities.BaseClockFrequencyKhz);
        }

        // Write it to the hive file now, so it survives holding the power button.
        ZwFlushKey(Key);
        ZwClose(Key);
    }
}
#endif

#ifdef ALLOC_PRAGMA
    #pragma alloc_text(INIT, DriverEntry)
#endif

//-----------------------------------------------------------------------------
// SlotExtension routines.
//-----------------------------------------------------------------------------

#if NV_T210_UHS
#ifndef NV_T210_UHS_DEFERRED
/* 0 = boot-time SDPORT profile (advertises UHS-I); 1 = off-boot-path worker. */
#define NV_T210_UHS_DEFERRED 0
#endif
#if NV_T210_UHS_DEFERRED
#ifndef NV_T210_UHS_STAGE
/* 0 = probe only (markers + read-only registers); 1+ adds the switch stages. */
#define NV_T210_UHS_STAGE 0
#endif
#endif
/* Compile-time gating only. This driver owns the boot disk, and querying the
   registry during DriverEntry / SdhcInitialize can deadlock the storage stack
   that is being initialised (observed twice as 0x7B INACCESSIBLE_BOOT_DEVICE
   on 2026-11-12). NV_T210_UHS_ACTIVE=1 turns the hardware path on. */
static ULONG T210Switch1V8, T210SwitchSdr104, T210SwitchMux;

/* Records the order in which SDPORT drives the UHS callbacks, so a single boot
   says who switched the voltage, when, and what came after. Four codes are
   packed per DWORD and written to the Diag key as UhsTrace0..3. */
static VOID
T210TraceAdd(
    _In_ PSDHC_EXTENSION SdhcExtension,
    _In_ ULONG Code
    )
{
    ULONG Slot;

    Slot = SdhcExtension->T210TraceCount;
    if (Slot >= RTL_NUMBER_OF(SdhcExtension->T210Trace) * 4) return;
    SdhcExtension->T210Trace[Slot / 4] |= (Code & 0xf) << ((Slot % 4) * 8);
    SdhcExtension->T210TraceCount = Slot + 1;
}

#if NV_T210_UHS_DMA
/* ---- DMA self-test scheduling -------------------------------------------
 * Runs ~45 s after the slot comes up: the boot itself must never depend on
 * DMA, and the test needs a quiet bus. Only an exact ADMA-vs-PIO match
 * enables DMA for the rest of the boot. */
static KTIMER T210DmaTimer;
static KDPC T210DmaDpc;
static WORK_QUEUE_ITEM T210DmaItem;

static VOID
T210DmaTestThread(
    _In_ PVOID Context
    )
{
    PSDHC_EXTENSION s = (PSDHC_EXTENSION)Context;
    LARGE_INTEGER delay;
    ULONG i;

    NvDiagCapture(L"UhsDmaBeat", 1);
    if (s != NULL) { NvDiagCapture(L"UhsDmaBeat2", 2); }
    delay.QuadPart = -1200000000LL;                      /* 120 s */
    KeDelayExecutionThread(KernelMode, FALSE, &delay);
    if (s == NULL) {
        PsTerminateSystemThread(STATUS_SUCCESS);
        return;
    }
    /* Only the controller inhibit bits say the bus is free; SDPORT may keep a
       request object queued (idle notification) the whole time. */
    for (i = 0; i < 6000; ++i) {                         /* up to ~60 s */
        if ((SdhcReadRegisterUlong(s, SDHC_PRESENT_STATE) &
             (SDHC_PS_CMD_INHIBIT | SDHC_PS_DAT_INHIBIT)) == 0) {
            break;
        }
        KeStallExecutionProcessor(10000);
    }
    {
        /* MC/SMMU translation-enable words: physical-address ADMA is only
           valid when the client translation is off. Read-only. */
        PHYSICAL_ADDRESS mcPa;
        PULONG mc;
        mcPa.QuadPart = 0x70019000;
        mc = (PULONG)MmMapIoSpaceEx(mcPa, PAGE_SIZE, PAGE_READONLY | PAGE_NOCACHE);
        if (mc != NULL) {
            s->T210Smmu = mc[0x0a88 / 4];
            s->T210Smmu2 = mc[0x0a8c / 4];
            s->T210Smmu3 = mc[0x0a90 / 4];
            s->T210Smmu4 = mc[0x0a94 / 4];
            MmUnmapIoSpace(mc, PAGE_SIZE);
        }
    }
    s->T210DmaTestOutstanding = (s->OutstandingRequests[0] != NULL) ? 1u : 0u;
#if NV_T210_DMA_PROBE_IN_DPC
    /* The verdict is taken from the request DPC instead (T210DmaProbe): on this
       machine the boot volume never left the bus quiet long enough for the old
       window search, so nothing is attempted here. */
    (VOID) i;
#else
    if (i >= 6000) {
        s->T210DmaTestResult = 0xE004;
    } else {
        T210DmaSelfTest(s);
    }
#endif
    NvDiagCapture(L"UhsDmaTest", s->T210DmaTestResult);
    NvDiagCapture(L"UhsDmaOk", s->T210DmaOk ? 1u : 0u);
    NvDiagCapture(L"UhsRca", s->T210Rca);
#if NV_DIAG_CLOCK
    NvDiagSaveClock(s);
#endif
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static VOID
T210DmaTestStarter(
    _In_ PVOID Context
    )
{
    HANDLE thread = NULL;

    if (Context == NULL) return;
    if (NT_SUCCESS(PsCreateSystemThread(&thread, THREAD_ALL_ACCESS, NULL, NULL, NULL,
                                        T210DmaTestThread, Context)) && (thread != NULL)) {
        ZwClose(thread);
    }
}

static VOID
T210DmaTestDpc(
    _In_ PKDPC Dpc,
    _In_opt_ PVOID Context,
    _In_opt_ PVOID Arg1,
    _In_opt_ PVOID Arg2
    )
{
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Arg1);
    UNREFERENCED_PARAMETER(Arg2);
    ExInitializeWorkItem(&T210DmaItem, T210DmaTestStarter, Context);
    ExQueueWorkItem(&T210DmaItem, DelayedWorkQueue);
}

static VOID
T210DmaTestSchedule(
    _In_ PSDHC_EXTENSION s
    )
{
    LARGE_INTEGER due;

    KeInitializeTimer(&T210DmaTimer);
    KeInitializeDpc(&T210DmaDpc, T210DmaTestDpc, s);
    due.QuadPart = -10000000LL * 120;
    KeSetTimer(&T210DmaTimer, due, &T210DmaDpc);
}
#endif /* NV_T210_UHS_DMA */
static VOID T210LoadSwitches(VOID)
{
#ifdef NV_T210_UHS_ACTIVE
    T210Switch1V8 = 1;
    T210SwitchSdr104 = 1;
    T210SwitchMux = 3;
#else
    T210Switch1V8 = 0;
    T210SwitchSdr104 = 0;
    T210SwitchMux = 0;
#endif
}
#endif

NTSTATUS
DriverEntry(
    _In_ PVOID DriverObject,
    _In_ PVOID RegistryPath
    )

/*++

Routine Description:

    This routine is the entry point for the standard sdhsot miniport driver.

Arguments:

    DriverObject - DriverObject for the standard host controller.

    RegistryPath - Registry path for this standard host controller.

Return Value:

    NTSTATUS

--*/

{

    // Keep absolute callback addresses in the image so the ARM64 loader can
    // relocate the miniport even when the optional diagnostics are disabled.
#if NV_T210_UHS
    T210LoadSwitches();
#endif
    static SDPORT_INITIALIZATION_DATA InitializationData = {
        .StructureSize = sizeof(InitializationData),
        .GetSlotCount = SdhcGetSlotCount,
        .GetSlotCapabilities = SdhcGetSlotCapabilities,
        .Initialize = SdhcSlotInitialize,
        .IssueBusOperation = SdhcSlotIssueBusOperation,
        .GetCardDetectState = SdhcSlotGetCardDetectState,
        .GetWriteProtectState = SdhcSlotGetWriteProtectState,
        .Interrupt = SdhcSlotInterrupt,
        .IssueRequest = SdhcSlotIssueRequest,
        .GetResponse = SdhcSlotGetResponse,
        .ToggleEvents = SdhcSlotToggleEvents,
        .ClearEvents = SdhcSlotClearEvents,
        .RequestDpc = SdhcRequestDpc,
        .SaveContext = SdhcSaveContext,
        .RestoreContext = SdhcRestoreContext,
        .PowerControlCallback = SdhcPoFxPowerControlCallback,
        .Cleanup = SdhcCleanup,
        .PrivateExtensionSize = sizeof(SDHC_EXTENSION),
        .CrashdumpSupported = !NV_T210_LITE_SLOT,
    };

#if NV_DIAG_STOP
    if (DriverObject != NULL) {
        LARGE_INTEGER Due;

        KeInitializeTimer(&NvDiag.Timer);
        KeInitializeDpc(&NvDiag.Dpc, NvDiagDpc, NULL);
        Due.QuadPart = -10000LL * NV_DIAG_DELAY_MS;
        KeSetTimer(&NvDiag.Timer, Due, &NvDiag.Dpc);
    }
#endif

#if NV_DIAG_REG || NV_DIAG_CLOCK
    // Loaded for crashdump or hibernation, DriverObject is NULL and the second
    // argument is not a registry path; reading it as one (0.2.2-0.2.5) is the
    // likely cause of their WHEA_UNCORRECTABLE_ERROR.
    if (DriverObject != NULL) {
        PUNICODE_STRING Service = (PUNICODE_STRING) RegistryPath;
        USHORT Chars = (Service != NULL) ? Service->Length / sizeof(WCHAR) : 0;

        if ((Service != NULL) && (Service->Buffer != NULL) &&
            (Chars + 20 < RTL_NUMBER_OF(NvDiag.RegistryPath))) {
            RtlCopyMemory(NvDiag.RegistryPath, Service->Buffer, Service->Length);
            RtlCopyMemory(&NvDiag.RegistryPath[Chars], L"\\Parameters\\Diag",
                          sizeof(L"\\Parameters\\Diag"));

#if NV_DIAG_REG
            {
                HANDLE Thread;

                if (NT_SUCCESS(PsCreateSystemThread(&Thread, THREAD_ALL_ACCESS, NULL, NULL,
                                                    NULL, NvDiagThread, NULL))) {
                    ZwClose(Thread);
                }
            }
#endif
        }
    }
#else
    UNREFERENCED_PARAMETER(RegistryPath);
#endif

    return SdPortInitialize(DriverObject, RegistryPath, &InitializationData);
}

NTSTATUS
SdhcGetSlotCount(
    _In_ PSD_MINIPORT Miniport,
    _Out_ PUCHAR SlotCount
    )

/*++

Routine Description:

    Return the number of slots present on this controller.

Arguments:

    Argument - Functional device object for this controller.

Return value:

    STATUS_UNSUCCESSFUL - PCI config space was unable to be queried.

    STATUS_INVALID_PARAMETER - Invalid underlying bus type for the miniport.

    STATUS_SUCCESS - SlotCount returned properly.

--*/

{

    SDPORT_BUS_TYPE BusType;
    UCHAR Data;
    NTSTATUS Status;

    *SlotCount = 0;
    BusType = Miniport->ConfigurationInfo.BusType;
    switch (BusType) {
    case SdBusTypeAcpi:

        //
        // We don't currently have a mechanism to query the slot count for ACPI
        // enumerated host controllers. Default to one slot.
        //

        *SlotCount = 1;
        Status = STATUS_SUCCESS;
        break;

    case SdBusTypePci:
        Data = 0;
        Status = SdPortGetPciConfigSpace(Miniport, 
                                         SDHC_PCICFG_SLOT_INFORMATION,
                                         &Data,
                                         sizeof(Data));

        if (!NT_SUCCESS(Status)) {
            return Status;
        }

        *SlotCount = (Data >> 4) + 1;
        if ((*SlotCount) > 6) {

            NT_ASSERT((*SlotCount) <= 6);

            Status = STATUS_UNSUCCESSFUL;
        }

        SdhcInitializePciConfigSpace(Miniport);
        Status = STATUS_SUCCESS;
        break;

    default:

        NT_ASSERT((BusType == SdBusTypeAcpi) || (BusType == SdBusTypePci));

        *SlotCount = 1;
        Status = STATUS_INVALID_PARAMETER;
        break;
    }

    return Status;
}

VOID
SdhcGetSlotCapabilities(
    _In_ PVOID PrivateExtension,
    _Out_ PSDPORT_CAPABILITIES Capabilities
    )

/*++

Routine Description:

    Override for miniport to provide host register mapping information if the
    memory range provideed by the underlying bus isn't sufficient.

Arguments:

    SlotExtension - SlotExtension interface between sdhost and this miniport.

    Capabilities - Miniport provided capabilities.

Return value:

    STATUS_SUCCESS - Capabilities returned successfully.

--*/

{

    PSDHC_EXTENSION SdhcExtension;

    SdhcExtension = (PSDHC_EXTENSION) PrivateExtension;
    RtlCopyMemory(Capabilities, 
                  &SdhcExtension->Capabilities,
                  sizeof(SdhcExtension->Capabilities));
}

NTSTATUS
SdhcSlotInitialize(
    _In_ PVOID PrivateExtension,
    _In_ PHYSICAL_ADDRESS PhysicalBase,
    _In_ PVOID VirtualBase,
    _In_ ULONG Length,
    _In_ BOOLEAN CrashdumpMode
    )

/*++

Routine Description:

    Initialize the miniport for standard host controllers.

Arguments:

    SlotExtension - SlotExtension interface between sdhost and this miniport.

    PhysicalBase - Physical base address of the host controller register space.

    VirtualBase - Mapped register space.

    Length - Length of the host register set in bytes.

    CrashdumpMode - whether this miniport is being used for crashdump.

Return value:

    NTSTATUS

--*/

{

    PSDPORT_CAPABILITIES Capabilities;
    SDHC_CAPABILITIES_REGISTER CapabilitiesReg;
    SDHC_CAPABILITIES2_REGISTER Capabilities2Reg;
    ULONG CurrentLimits;
    ULONG CurrentLimitMax;
    ULONG CurrentLimitMask;
    ULONG CurrentLimitShift;
    UCHAR Index;
    PSDHC_EXTENSION SdhcExtension;
    USHORT SpecVersion;

    SdhcExtension = (PSDHC_EXTENSION) PrivateExtension;

    //
    // Initialize the SDHC_EXTENSION register space.
    //

#if NV_T210_LITE_WINARM_WRITE
    LiteForgetCard(SdhcExtension);
#endif
    SdhcExtension->PhysicalBaseAddress = PhysicalBase;
    SdhcExtension->BaseAddress = VirtualBase;
    SdhcExtension->BaseAddressSpaceSize = Length;
    SdhcExtension->BaseAddressDebug = 
        (PSD_HOST_CONTROLLER_REGISTERS) VirtualBase;

    //
    // Check whether the driver is in crashdump mode.
    //

    SdhcExtension->CrashdumpMode = CrashdumpMode;
#if NV_T214_SDR104
    if (CrashdumpMode && KeGetCurrentIrql() != PASSIVE_LEVEL) return STATUS_NOT_SUPPORTED;
#endif
#if NV_T210_UHS
    /* T210 (Erista) UHS-I. The switches were read in DriverEntry (PASSIVE);
       this callback may run at DISPATCH_LEVEL, so nothing here touches the
       registry. Everything stays inert unless Parameters\Uhs1V8 is 1. */
#if NV_T210_UHS_DEFERRED
    /* Deferred profile: boot keeps the verified 3.3 V high-speed path. No
       UHS work and no SDR104 capability here; the late worker attempts the
       switch after the system is up. */
    SdhcExtension->T210UhsMux = 3;
    SdhcExtension->T210Sdr104 = FALSE;
    SdhcExtension->T210Enabled = FALSE;
    NvDiagCapture(L"UhsDeferred", 1);
    NvT210ScheduleLate(SdhcExtension);
#else
    SdhcExtension->T210UhsMux = T210SwitchMux;
    SdhcExtension->T210Sdr104 = T210SwitchSdr104 != 0;
    SdhcExtension->T210Enabled = (T210Switch1V8 != 0) || SdhcExtension->T210Sdr104;
    if (SdhcExtension->T210Enabled) {
        NTSTATUS uhs = T210UhsInit(SdhcExtension);
        if (!NT_SUCCESS(uhs)) {
            SdhcExtension->T210Enabled = FALSE;
            SdhcExtension->T210Sdr104 = FALSE;
        }
        SdhcExtension->T210InitStatus = (ULONG)uhs;
    }
#if NV_T210_UHS_SLOT_CAR
    /* Do what T214Initialize does: move SDMMC1 onto PLLC4 and advertise the
       199.68 MHz base clock before SDPORT negotiates the bus. Without a high
       base clock SDPORT cannot reach an SDR104 rate and abandons UHS. */
    if (SdhcExtension->T210Enabled) {
        NTSTATUS car = T210ClockSdr104(SdhcExtension);
        SdhcExtension->T210InitStatus = (ULONG)car;
        NvDiagCapture(L"UhsSlotCar", (ULONG)car);
        NvDiagCapture(L"UhsBaseKhz", SdhcExtension->T210BaseKhz);
        NvDiagCapture(L"UhsCarSource", SdhcExtension->T210CarSource);
    }
#endif
#endif
#if NV_T210_UHS_DMA
    /* DMA self-test a bit after boot; only a passing test enables it. */
    if (SdhcExtension->T210Enabled) T210DmaTestSchedule(SdhcExtension);
#endif
    NvDiagCapture(L"UhsFlags", (SdhcExtension->T210Enabled ? 1u : 0u) | (SdhcExtension->T210Sdr104 ? 2u : 0u));
    NvDiagCapture(L"UhsMuxValue", SdhcExtension->T210UhsMux);
#endif

    //
    // Initialize host capabilities.
    //

    Capabilities = (PSDPORT_CAPABILITIES) &SdhcExtension->Capabilities;
    CapabilitiesReg.AsUlong =
        SdhcReadRegisterUlong(SdhcExtension, SDHC_CAPABILITIES);

    Capabilities2Reg.AsUlong =
        SdhcReadRegisterUlong(SdhcExtension, SDHC_CAPABILITIES2);

    SpecVersion = SdhcReadRegisterUshort(SdhcExtension, SDHC_VERSION);
    Capabilities->SpecVersion = SpecVersion & 0xFF;
    Capabilities->MaximumOutstandingRequests = SDHC_MAX_OUTSTANDING_REQUESTS;
    Capabilities->MaximumBlockSize =
        (USHORT)(512 << CapabilitiesReg.MaxBlockLength);

    Capabilities->MaximumBlockCount = 0xFFFF;
    Capabilities->BaseClockFrequencyKhz =
        CapabilitiesReg.BaseClockFrequency * 1000;

#if NV_T210_LITE_SLOT
    {
        PHYSICAL_ADDRESS CarAddress;
        PULONG CarBase;
        ULONG Source;

        if (PhysicalBase.QuadPart != 0x700B0000) {
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        }
        CarAddress.QuadPart = NV_T210_CAR_BASE;
        CarBase = MmMapIoSpaceEx(CarAddress, PAGE_SIZE, PAGE_READONLY | PAGE_NOCACHE);
        if (CarBase == NULL) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        Source = READ_REGISTER_ULONG(&CarBase[NV_T210_CLK_SOURCE_SDMMC1 / sizeof(ULONG)]);
        MmUnmapIoSpace(CarBase, PAGE_SIZE);
        if ((Source >> 29) != 0 && !(NV_T214_SDR104 && (Source >> 29) == 3)) {
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        }
        // Hekate/UEFI left SDMMC1 on PLLP_OUT0 (408 MHz), with a 7.1 divider.
        Capabilities->BaseClockFrequencyKhz = (2u * ((Source >> 29) == 3 ? 199680u : 408000u)) / ((Source & 0xffu) + 2u);
        NvDiag.CarSdmmc1Before = Source;
        NvDiag.CarSdmmc1After = Source;
    }
#elif NV_T210_FIXED_CAR_CLOCK
    //
    // The controller reports 208 MHz, but its real input is the CAR
    // SDMMC1 clock, which the UEFI leaves at whatever card clock it used
    // last (tens of MHz). Dividing for 208 MHz then gave the card about a
    // sixth of the intended clock (2.6 MB/s). Set the CAR clock to a known
    // rate and divide from that. Not in crashdump mode: the normal load
    // already set it.
    //

    BOOLEAN CarSet = (BOOLEAN) CrashdumpMode;

    if (!CrashdumpMode) {
        PHYSICAL_ADDRESS Car;
        PULONG CarBase;
        USHORT ClockControl;

        ClockControl = SdhcReadRegisterUshort(SdhcExtension, SDHC_CLOCK_CONTROL);
        SdhcWriteRegisterUshort(SdhcExtension,
                                SDHC_CLOCK_CONTROL,
                                ClockControl & ~SDHC_CC_CLOCK_ENABLE);

        Car.QuadPart = NV_T210_CAR_BASE;
        CarBase = (PULONG) MmMapIoSpaceEx(Car, PAGE_SIZE,
                                         PAGE_READWRITE | PAGE_NOCACHE);
        if (CarBase != NULL) {
            NvDiag.CarSdmmc1Before =
                READ_REGISTER_ULONG(&CarBase[NV_T210_CLK_SOURCE_SDMMC1 / 4]);
#if NV_T210_CAR_DIVISOR != NV_T210_CAR_DIVISOR_NONE
            WRITE_REGISTER_ULONG(&CarBase[NV_T210_CLK_SOURCE_SDMMC1 / 4],
                                 ((ULONG) NV_T210_CAR_DIVISOR) & 0xFFu);
#else
            WRITE_REGISTER_ULONG(&CarBase[NV_T210_CLK_SOURCE_SDMMC1 / 4],
                                 NV_T210_SDMMC1_CAR_SOURCE);
#endif
            KeStallExecutionProcessor(10);
            NvDiag.CarSdmmc1After =
                READ_REGISTER_ULONG(&CarBase[NV_T210_CLK_SOURCE_SDMMC1 / 4]);
            MmUnmapIoSpace(CarBase, PAGE_SIZE);
            CarSet = TRUE;
        }
    }

    if (CarSet) {
#if NV_T210_CAR_DIVISOR != NV_T210_CAR_DIVISOR_NONE
        /* Report the rate the controller really receives, so the SDCLK the
           driver programs (see SdhcCalcClockFrequency) is the real card clock. */
        Capabilities->BaseClockFrequencyKhz =
            NV_T210_PLLP_KHZ / (((ULONG) NV_T210_CAR_DIVISOR) + 2u);
#else
        Capabilities->BaseClockFrequencyKhz = NV_T210_SDMMC1_CAR_KHZ;
#endif
#if NV_T210_UHS
    /* Declare UHS-I only when Parameters\Uhs1V8 (or UhsSdr104) is set: SDPORT
       then drives the whole sequence itself (signalling, speed, tuning).
       Deferred profile keeps the boot path on plain 3.3 V high speed. */
    if (SdhcExtension->T210Enabled && !NV_T210_UHS_DEFERRED && !NV_T210_UHS_NO_ADVERTISE) {
        Capabilities->Supported.HighSpeed = 1;
        Capabilities->Supported.SignalingVoltage18V = 1;
        Capabilities->Supported.SDR50 = 0;
        Capabilities->Supported.DDR50 = 0;
        if (SdhcExtension->T210Sdr104) Capabilities->Supported.SDR104 = 1;
        if (SdhcExtension->T210BaseKhz) Capabilities->BaseClockFrequencyKhz = SdhcExtension->T210BaseKhz + 2000;

    }
#endif
    }
#endif

#if NV_T214_SDR104
    if (CrashdumpMode || Length < 0x200) return STATUS_NOT_SUPPORTED;
    NTSTATUS t214Status = T214Initialize(SdhcExtension);
    if (!NT_SUCCESS(t214Status)) return t214Status;
#endif
#if NV_T210_ADMA32
    CapabilitiesReg.SystemBus64Support = 0;
#endif

    Capabilities->DmaDescriptorSize =
        sizeof(SDHC_ADMA2_DESCRIPTOR_TABLE_ENTRY) +
        (CapabilitiesReg.SystemBus64Support ?
        sizeof(ULONGLONG) : sizeof(ULONG));

    Capabilities->AlignmentRequirement =
        (CapabilitiesReg.SystemBus64Support ?
            sizeof(ULONGLONG) : sizeof(ULONG)) - 1;

    //
    // For small transfers (SDIO) we want to use PIO for performance,
    // for both reads and writes <= 64 bytes.
    //

    Capabilities->PioTransferMaxThreshold = 64;
    Capabilities->Flags.UsePioForRead = TRUE;
    Capabilities->Flags.UsePioForWrite = TRUE;

#if NV_T210_PIO_ONLY || NV_T210_LITE_SLOT
    Capabilities->PioTransferMaxThreshold = MAXULONG;
#else
    if (CrashdumpMode) {
        Capabilities->PioTransferMaxThreshold = MAXULONG;

    } else if (CapabilitiesReg.Adma2Support) {
        Capabilities->Supported.ScatterGatherDma = 1;
    }
#endif

    if (CapabilitiesReg.SystemBus64Support) {
        Capabilities->Supported.Address64Bit = 1;
    }

#if NV_T210_UHS_DMA
    /* Keep SDPORT's requests inside the bounce buffer; anything larger would
       fall back to PIO and crawl. */
    if (SdhcExtension->T210Dma != NULL) {
        Capabilities->MaximumBlockCount = (USHORT)(SdhcExtension->T210DmaCapacity / 512);
    }
#endif

    if (!NV_T210_LITE_SLOT && CapabilitiesReg.Support8BitBus) {
        Capabilities->Supported.BusWidth8Bit = 1;
    }

    if ((!NV_T210_LITE_SLOT || NV_T214_SDR104) && CapabilitiesReg.HighSpeedSupport) {
        Capabilities->Supported.HighSpeed = 1;
    }

    if (Capabilities2Reg.SDR50Support) {
        Capabilities->Supported.SDR50 = 1;
        Capabilities->Supported.SignalingVoltage18V = 1;
    }

    if (Capabilities2Reg.DDR50Support) {
        Capabilities->Supported.DDR50 = 1;
        Capabilities->Supported.SignalingVoltage18V = 1;
    }

    if (Capabilities2Reg.SDR104Support) {
        Capabilities->Supported.SDR104 = 1;
        Capabilities->Supported.SignalingVoltage18V = 1;
    }

#if NV_T210_SAFE_BRINGUP
    Capabilities->Supported.SDR50 = 0;
    Capabilities->Supported.DDR50 = 0;
    Capabilities->Supported.SDR104 = 0;
    Capabilities->Supported.SignalingVoltage18V = 0;
#endif

#if NV_T214_SDR104
    Capabilities->Supported.SDR50 = 0;
    Capabilities->Supported.DDR50 = 0;
    Capabilities->Supported.SDR104 = 1;
    Capabilities->Supported.SignalingVoltage18V = 1;
#endif
#if NV_T210_UHS
    /* T210 (Erista): declare UHS-I *after* the SAFE_BRINGUP zeroing and the
       T214 re-enable. The earlier T210 block sits before that zeroing and is
       wiped there, so SDPORT silently stayed at 3.3 V high speed (observed
       on hardware 2026-11-22: BusSpeed=2, no CMD6/CMD11 attempt). Deferred
       profile must not advertise here either. */
    if (SdhcExtension->T210Enabled && !NV_T210_UHS_DEFERRED && !NV_T210_UHS_NO_ADVERTISE) {
        Capabilities->Supported.HighSpeed = 1;
        Capabilities->Supported.SignalingVoltage18V = 1;
        Capabilities->Supported.SDR50 = 0;
        Capabilities->Supported.DDR50 = 0;
#if NV_T210_UHS_NO_SDR104
        /* Experiment: advertise 1.8 V signalling but no SDR104, so SDPORT picks
           the 1.8 V "SDR25" mode that needs no tuning. Isolates "can the rail
           stay at 1.8 V" from the tuning problem. */
        Capabilities->Supported.SDR104 = 0;
#else
        Capabilities->Supported.SDR104 =
            (SdhcExtension->T210Sdr104 ? 1 : 0);
#endif
        if (SdhcExtension->T210BaseKhz) {
            Capabilities->BaseClockFrequencyKhz =
                SdhcExtension->T210BaseKhz + 2000;
        }
    }
#endif
    Capabilities->Supported.HS200 = 0;
    Capabilities->Supported.HS400 = 0;
#if NV_T210_UHS && NV_T210_UHS_NO_ADVERTISE
    /* The generic path above still advertises UHS from the hardware capability
       registers (SDR104Support/SDR50Support). The pure high-speed DMA profile
       needs them off too, otherwise SDPORT runs the 1.8 V attempt this card
       cannot complete. */
    Capabilities->Supported.SignalingVoltage18V = 0;
    Capabilities->Supported.SDR50 = 0;
    Capabilities->Supported.DDR50 = 0;
    Capabilities->Supported.SDR104 = 0;
    Capabilities->Supported.SoftwareTuning = 0;
    Capabilities->Supported.TuningForSDR50 = 0;
#endif

    if (Capabilities2Reg.DriverTypeA) {
        Capabilities->Supported.DriverTypeA = 1;
    }

    if (Capabilities2Reg.DriverTypeC) {
        Capabilities->Supported.DriverTypeC = 1;
    }

    if (Capabilities2Reg.DriverTypeD) {
        Capabilities->Supported.DriverTypeD = 1;
    }

#if NV_T214_SDR104
    Capabilities->Supported.DriverTypeA = 0;
    Capabilities->Supported.DriverTypeC = 0;
    Capabilities->Supported.DriverTypeD = 0;
#endif
    Capabilities->Supported.DriverTypeB = 1;

    if (Capabilities2Reg.UseTuningForSDR50) {
        Capabilities->Supported.TuningForSDR50 = 1;
    }

    if (Capabilities2Reg.RetuningTimerCount != 0) {
        Capabilities->TuningTimerCountInSeconds =
            (1 << (Capabilities2Reg.RetuningTimerCount - 1));
    }

    if (Capabilities2Reg.RetuningModes == 0) {
        Capabilities->Supported.SoftwareTuning = 1;
    }

#if NV_T210_SAFE_BRINGUP
    Capabilities->Supported.TuningForSDR50 = 0;
    Capabilities->Supported.SoftwareTuning = 0;
#endif

#if NV_T210_UHS
    /* SDR104 always needs a tuning pass. SAFE_BRINGUP above clears the flag
       on this build (it is !NV_T214_SDR104), and with SoftwareTuning = 0
       SDPORT silently skips SdExecuteTuning: the card then runs SDR104 with
       an untuned sampling point and the first data reads fail (observed
       2026-11-22: boot died after a verified 1.8 V switch). */
    if (SdhcExtension->T210Enabled && !NV_T210_UHS_DEFERRED && !NV_T210_UHS_NO_ADVERTISE) {
        Capabilities->Supported.SoftwareTuning = 1;
        if (Capabilities->TuningTimerCountInSeconds == 0) {
            Capabilities->TuningTimerCountInSeconds = 4;
        }
        /* UHS modes want the 1.8 V current budget declared explicitly: the
           generic path above derives the Limit* flags from the 3.3 V field of
           MAXIMUM_CURRENT only, and SDR104 needs 800 mA at 1.8 V. T214 forces
           them the same way. */
        Capabilities->Supported.Limit800mA = 1;
        Capabilities->Supported.Limit600mA = 1;
        Capabilities->Supported.Limit400mA = 1;
        Capabilities->Supported.Limit200mA = 1;
        NvDiagCapture(L"UhsMaxCurrent", SdhcReadRegisterUlong(SdhcExtension, SDHC_MAXIMUM_CURRENT));
    }
#endif

#if NV_T214_SDR104
    Capabilities->Supported.SoftwareTuning = 1;
    Capabilities->Supported.TuningForSDR50 = 0;
    Capabilities->TuningTimerCountInSeconds = 4;
    Capabilities->Supported.Limit800mA = 1;
    Capabilities->Supported.Limit600mA = 1;
    Capabilities->Supported.Limit400mA = 1;
    Capabilities->Supported.Limit200mA = 1;
#endif
    Capabilities->Supported.AutoCmd12 = 1;
    if (!NV_T214_SDR104 && SpecVersion >= SDHC_SPEC_VERSION_3) {
        Capabilities->Supported.AutoCmd23 = 1;
    }

    if (!NV_T210_LITE_SLOT && CapabilitiesReg.Voltage18) {
        Capabilities->Supported.Voltage18V = 1;
    }

    if (!NV_T210_LITE_SLOT && CapabilitiesReg.Voltage30) {
        Capabilities->Supported.Voltage30V = 1;
    }

    if (CapabilitiesReg.Voltage33) {
        Capabilities->Supported.Voltage33V = 1;
    }

    //
    // Find the current limits suppported by the controller.
    //

    CurrentLimitMask = 0;
    CurrentLimitShift = 0;
    if (Capabilities->Supported.Voltage33V) {
        CurrentLimitMask = 0xFF;
        CurrentLimitShift = 0;

    } else if (Capabilities->Supported.Voltage30V) {
        CurrentLimitMask = 0xFF00;
        CurrentLimitShift = 8;

    } else if (Capabilities->Supported.Voltage18V) {
        CurrentLimitMask = 0xFF0000;
        CurrentLimitShift = 16;
    }

    CurrentLimits = SdhcReadRegisterUlong(SdhcExtension, SDHC_MAXIMUM_CURRENT);
    CurrentLimitMax = 
        ((CurrentLimits & CurrentLimitMask) >> CurrentLimitShift) * 4;

    if (CurrentLimitMax >= 800) {
        Capabilities->Supported.Limit800mA = 1;
    }

    if (CurrentLimitMax >= 600) {
        Capabilities->Supported.Limit600mA = 1;
    }

    if (CurrentLimitMax >= 400) {
        Capabilities->Supported.Limit400mA = 1;
    }

    if (CurrentLimitMax >= 200) {
        Capabilities->Supported.Limit200mA = 1;
    }

    //
    // Initialize our array of outstanding requests to keep track
    // of what operations are in flight.
    //

    for (Index = 0; 
         Index < Capabilities->MaximumOutstandingRequests;
         Index += 1) {

        RtlZeroMemory(&SdhcExtension->OutstandingRequests[Index],
                      sizeof(PSDPORT_REQUEST));
    }

    NvDiag.PresentState =
        SdhcReadRegisterUlong(SdhcExtension, SDHC_PRESENT_STATE);

    if (NvDiag.PresentState & SDHC_PS_CARD_INSERTED) {
        NvDiag.Flags |= 0x1;
    }

    if (NvDiag.PresentState & SDHC_PS_CARD_STATE_STABLE) {
        NvDiag.Flags |= 0x2;
    }

    NvDiag.RegCapabilities = CapabilitiesReg.AsUlong;
    NvDiag.RegCapabilities2 = Capabilities2Reg.AsUlong;
    NvDiag.Extension = SdhcExtension;
    NvDiag.Initialized = TRUE;
#if NV_DIAG_CLOCK
    /* Arm the one-shot late gate; no diagnostics run before it fires. */
    NvDiagScheduleLate(SdhcExtension);
#endif
    return STATUS_SUCCESS;
}

NTSTATUS
SdhcSlotIssueBusOperation(
    _In_ PVOID PrivateExtension,
    _In_ PSDPORT_BUS_OPERATION BusOperation
    )

/*++

Routine Description:

    Issue host bus operation specified by BusOperation.

Arguments:

    SlotExtension - SlotExtension interface between sdhost and this miniport.

    BusOperation - Bus operation to perform.

Return value:

    NTSTATUS - Return value of the bus operation performed.

--*/

{

    PSDHC_EXTENSION SdhcExtension;
    NTSTATUS Status;

    Status = STATUS_INVALID_PARAMETER;
    SdhcExtension = (PSDHC_EXTENSION) PrivateExtension;
#if NV_T214_DDR200
    if (SdhcExtension->T214DdrRecoverNeeded &&
        (BusOperation->Type == SdResetHost || BusOperation->Type == SdExecuteTuning)) {
        Status = T214DdrRecover(SdhcExtension);
#if NV_DIAG_CLOCK
        NvDiagSaveClock(SdhcExtension);
#endif
        /* Recovery already reset the host, re-enumerated the card and
         * re-tuned SDR104. A second reset would discard its verified state.
         */
        return Status;
    }
#endif
    switch (BusOperation->Type) {
    case SdResetHost:
        Status = SdhcResetHost(SdhcExtension,
                               BusOperation->Parameters.ResetType);

        if (!NT_SUCCESS(Status)) {
            NvDiag.Flags |= 0x8;
        }

        break;

    case SdSetClock:
        Status = SdhcSetClock(SdhcExtension,
                              BusOperation->Parameters.FrequencyKhz);

        break;

    case SdSetVoltage:
        Status = SdhcSetVoltage(SdhcExtension,
                                BusOperation->Parameters.Voltage);

        break;

    case SdSetBusWidth:
        Status = SdhcSetBusWidth(SdhcExtension,
                                 BusOperation->Parameters.BusWidth);

        NvDiag.BusWidth = BusOperation->Parameters.BusWidth;
        NvDiag.RegHostControl =
            SdhcReadRegisterUchar(SdhcExtension, SDHC_HOST_CONTROL);
#if NV_DIAG_CLOCK
        NvDiagSaveClock(SdhcExtension);
#endif

        break;

    case SdSetBusSpeed:
        Status = SdhcSetSpeed(SdhcExtension, BusOperation->Parameters.BusSpeed);
        NvDiag.BusSpeed = BusOperation->Parameters.BusSpeed;
        NvDiag.RegHostControl =
            SdhcReadRegisterUchar(SdhcExtension, SDHC_HOST_CONTROL);
#if NV_DIAG_CLOCK
        NvDiagSaveClock(SdhcExtension);
#endif
        break;

    case SdSetSignalingVoltage:
        Status = 
            SdhcSetSignaling(
                SdhcExtension, 
                (BOOLEAN)(BusOperation->Parameters.SignalingVoltage == 
                          SdSignalingVoltage18));

        break;

    case SdSetDriveStrength:
#if NV_T214_SDR104
        // Fixed board supply supports up to 800mA (Switchroot B01 DT).
        Status = (BusOperation->Parameters.DriveStrength >= SdDriveStrength200mA &&
                  BusOperation->Parameters.DriveStrength <= SdDriveStrength800mA) ?
                  STATUS_SUCCESS : STATUS_NOT_SUPPORTED;
#endif
        //Status = SdhcSetDriveStrength(SdhcExtension,
        //                            BusOperation->Parameters.DriveStrength);
        break;

    case SdSetDriverType:
#if NV_T214_SDR104
        Status = STATUS_NOT_SUPPORTED;
        if (BusOperation->Parameters.DriverType == SdDriverTypeB) {
            USHORT control = SdhcReadRegisterUshort(SdhcExtension, SDHC_HOST_CONTROL2);
            SdhcWriteRegisterUshort(SdhcExtension, SDHC_HOST_CONTROL2, control & ~0x30u);
            Status = STATUS_SUCCESS;
        }
#endif
        //Status = SdhcSetDriverType(SdhcExtension,
        //                         BusOperation->Parameters.DriverType);

        break;

    case SdSetPresetValue:
        Status = SdhcSetPresetValue(
                    SdhcExtension,
                    BusOperation->Parameters.PresetValueEnabled);

        break;

    case SdSetBlockGapInterrupt:
        Status = SdhcEnableBlockGapInterrupt(
                    SdhcExtension,
                    BusOperation->Parameters.BlockGapIntEnabled);

        break;

    case SdExecuteTuning:
        Status = SdhcExecuteTuning(SdhcExtension);
        break;

    default:
        Status = STATUS_INVALID_PARAMETER;
        break; 
    }

    return Status;
}

BOOLEAN
SdhcSlotGetCardDetectState(
    _In_ PVOID PrivateExtension
    )

/*++

Routine Description:

    Determine whether a card is inserted in the slot.

Arguments:

    SlotExtension - SlotExtension interface between sdhost and this miniport.

Return value:

    TRUE - Card is inserted.

    FALSE - Slot is empty.

--*/

{

    PSDHC_EXTENSION SdhcExtension;

    SdhcExtension = (PSDHC_EXTENSION) PrivateExtension;

    // The Switch reports card detect on a GPIO, not reliably in the SDHCI
    // present state. Windows boots from this card, so it is always there.
    UNREFERENCED_PARAMETER(SdhcExtension);
    return TRUE;
}

BOOLEAN
SdhcSlotGetWriteProtectState(
    _In_ PVOID PrivateExtension
    )

/*++

Routine Description:

    Determine whether the slot write protection is engaged.

Arguments:

    SlotExtension - SlotExtension interface between sdhost and this miniport.

Return value:

    TRUE - Slot is write protected.

    FALSE - Slot is writable.

--*/

{
    // Tegra210's SDHCI WRITE_PROTECT bit is not wired reliably.  Linux uses
    // the board GPIO when available and otherwise assumes write-enabled.
    // The Lite read-only build enforces write protection in IssueRequest.
    UNREFERENCED_PARAMETER(PrivateExtension);
    return NV_T210_LITE_READ_ONLY;
}

BOOLEAN
SdhcSlotInterrupt(
    _In_ PVOID PrivateExtension,
    _Out_ PULONG Events,
    _Out_ PULONG Errors,
    _Out_ PBOOLEAN CardChange,
    _Out_ PBOOLEAN SdioInterrupt,
    _Out_ PBOOLEAN Tuning
    )

/*++

Routine Description:

    Level triggered DIRQL interrupt handler (ISR) for this controller.

Arguments:

    SlotExtension - SlotExtension interface between sdhost and this miniport.

Return value:

    Whether the interrupt is handled.

--*/

{

    PSDHC_EXTENSION SdhcExtension;

    SdhcExtension = (PSDHC_EXTENSION) PrivateExtension;
    *Events = (ULONG) SdhcGetInterruptStatus(SdhcExtension);

    //
    // If there aren't any events to handle, then we don't need to
    // process anything.
    //

    if (*Events == 0) {
        return FALSE;
    }

    NvDiag.Flags |= 0x4;
    NvDiag.Interrupts += 1;
    if (*Events & SDHC_IS_ERROR_INTERRUPT) {
        NvDiag.ErrorInterrupts += 1;
        *Errors = (ULONG) SdhcGetErrorStatus(SdhcExtension);
    }

    //
    // If a card has changed, notify the port driver.
    //

    if (*Events & SDHC_IS_CARD_DETECT) {
#if NV_T210_LITE_WINARM_WRITE
        LiteForgetCard(SdhcExtension);
#endif
        *CardChange = TRUE;
    }

    //
    // If we have an external SDIO interrupt, notify the port driver.
    //
    
    if (*Events & SDHC_IS_CARD_INTERRUPT) {
        *SdioInterrupt = TRUE; 
    }

    //
    // If there's a tuning request, notify the port driver.
    //

    if (*Events & SDHC_IS_TUNING_INTERRUPT) {
        *Tuning = TRUE;
    }

    //
    // Acknowledge/clear interrupt status. Request completions will occur in
    // the port driver's slot completion DPC. We need to make the members of 
    // SDPORT_REQUEST that only the port driver uses opaque to the miniport.
    // See how Storport does this (if it does).
    //

    SdhcAcknowledgeInterrupts(SdhcExtension, (USHORT) *Events);
    *Events &= ~(SDHC_IS_CARD_DETECT |
                 SDHC_IS_CARD_INTERRUPT |
                 SDHC_IS_TUNING_INTERRUPT);

    return (*Events != 0) || (*CardChange) || (*SdioInterrupt) || (*Tuning);
}

NTSTATUS
SdhcSlotIssueRequest(
    _In_ PVOID PrivateExtension,
    _In_ PSDPORT_REQUEST Request
    )

/*++

Routine Description:

    Issue hardware request specified by Request.

Arguments:

    PrivateExtension - This driver's device extension (SdhcExtension).

    Request - Request operation to perform.

Return value:

    NTSTATUS - Return value of the request performed.

--*/

{

    ULONG Index;
    PSDHC_EXTENSION SdhcExtension;
    NTSTATUS Status;

    SdhcExtension = (PSDHC_EXTENSION) PrivateExtension;
#if NV_T214_DDR200
    if (SdhcExtension->T214DdrFatal &&
        (Request->Command.Index == 17 || Request->Command.Index == 18 ||
         Request->Command.Index == 24 || Request->Command.Index == 25))
        return STATUS_DEVICE_NOT_READY;
#endif
#if NV_T210_LITE_WINARM_WRITE
    if (!LiteAllowRequest(SdhcExtension, Request)) {
        NvDiag.DeniedCount += 1;
        NvDiag.LastDeniedCommand = Request->Command.Index;
        NvDiag.LastDeniedArgument = Request->Command.Argument;
        NvDiag.DeniedShape = ((ULONG)Request->Type << 16) |
                             ((ULONG)Request->Command.Class << 8) |
                             (ULONG)Request->Command.TransferDirection;
#if NV_DIAG_CLOCK
        if (NvDiag.DeniedCount <= 4) {
            NvDiagSaveClock(SdhcExtension);
        }
#endif
        return STATUS_MEDIA_WRITE_PROTECTED;
    }
    if (Request->Command.Index == 0) {
        LiteForgetCard(SdhcExtension);
    }
#elif NV_T210_LITE_READ_ONLY
    // Refuse writes before reserving an outstanding request or touching a register.
    if ((Request->Type == SdRequestTypeCommandWithTransfer ||
         Request->Type == SdRequestTypeStartTransfer) &&
        Request->Command.TransferDirection != SdTransferDirectionRead) {
        return STATUS_MEDIA_WRITE_PROTECTED;
    }
    switch (Request->Command.Index) {
    case 24:  // WRITE_BLOCK
    case 25:  // WRITE_MULTIPLE_BLOCK
    case 26:  // PROGRAM_CID
    case 27:  // PROGRAM_CSD
    case 28:  // SET_WRITE_PROT
    case 29:  // CLR_WRITE_PROT
    case 38:  // ERASE
        return STATUS_MEDIA_WRITE_PROTECTED;
    }
#endif

    //
    // Insert the request onto the outstanding requests list.
    //

    for (Index = 0;
         Index < SdhcExtension->Capabilities.MaximumOutstandingRequests;
         Index += 1) {

        if (SdhcExtension->OutstandingRequests[Index] == NULL) {
            SdhcExtension->OutstandingRequests[Index] = Request;
            break;
        }
    }

    //
    // Dispatch the request based off of the request type.
    //

    if (Request->Type == SdRequestTypeCommandWithTransfer) {
        LARGE_INTEGER Frequency;
        LONGLONG Now;

        Now = KeQueryPerformanceCounter(&Frequency).QuadPart;
        if ((NvDiag.DataEnd != 0) && (Frequency.QuadPart != 0)) {
            NvDiag.GapUs += (ULONG) ((Now - NvDiag.DataEnd) * 1000000 / Frequency.QuadPart);
        }

        NvDiag.DataRequest = Request;
        NvDiag.DataStart = Now;
        NvDiag.DataStartBlocks = Request->Command.BlockCount;
    }

    switch (Request->Type) {
    case SdRequestTypeCommandNoTransfer:
    case SdRequestTypeCommandWithTransfer:
        Status = SdhcSendCommand(SdhcExtension, Request);
        break;

    case SdRequestTypeStartTransfer:
        Status = SdhcStartTransfer(SdhcExtension, Request);
        break;

    default:
        Status = STATUS_NOT_SUPPORTED;
        break;
    }

    if (Request->Type != SdRequestTypeStartTransfer) {
        NvDiag.Commands += 1;
        NvDiag.LastCommand = Request->Command.Index;
    }

    if (!NT_SUCCESS(Status)) {
        NvDiag.LastErrorCommand = Request->Command.Index;
        NvDiag.LastErrorKind = 8;
        if (Index < SDHC_MAX_OUTSTANDING_REQUESTS) SdhcExtension->OutstandingRequests[Index] = NULL;
        if (NvDiag.DataRequest == Request) NvDiag.DataRequest = NULL;
    }

    return Status;
}

VOID
SdhcSlotGetResponse(
    _In_ PVOID PrivateExtension,
    _In_ PSDPORT_COMMAND Command,
    _Out_ PVOID ResponseBuffer
    )

/*++

Routine Description:

    Return the response data for a given command back to the port driver.

Arguments:

    PrivateExtension - This driver's device extension (SdhcExtension).

    Command - Command for which we're getting the response.

    ResponseBuffer - Response data for the given command.

Return value:

    None.

--*/

{

    PSDHC_EXTENSION SdhcExtension;
    NTSTATUS Status;

    SdhcExtension = (PSDHC_EXTENSION) PrivateExtension;
    Status = SdhcGetResponse(SdhcExtension, Command, ResponseBuffer);
#if NV_T214_DDR200
    if (NT_SUCCESS(Status) && Command->Index == 3 && Command->ResponseType == SdResponseTypeR6) {
        ULONG r6;
        RtlCopyMemory(&r6, ResponseBuffer, sizeof(r6));
        if (!(r6 & 0xe000u)) SdhcExtension->T214Rca = r6 & 0xffff0000u;
    } else if (NT_SUCCESS(Status) && Command->Index == 2 && Command->ResponseType == SdResponseTypeR2) {
        RtlCopyMemory(SdhcExtension->T214Cid, ResponseBuffer, 16);
        SdhcExtension->T214CardInfoValid |= 4;
    }
#endif
#if NV_T210_UHS
    /* Record the card's OCR both before WINDOW_SWITCH and while the 1.8 V switch
       window is open, so one boot shows whether S18A (bit 24) differs between
       3.3 V and 1.8 V phases. Read-only: nothing is modified. */
    if (NT_SUCCESS(Status) && SdhcExtension->T210Enabled &&
        Command->Index == 41 && Command->ResponseType == SdResponseTypeR3) {
        ULONG ocr = *(PULONG)ResponseBuffer;
        if (!SdhcExtension->T210UhsWindow) {
            if (SdhcExtension->T210OcrFirstCount == 0) SdhcExtension->T210OcrFirst = ocr;
            SdhcExtension->T210OcrFirstCount++;
        } else {
            SdhcExtension->T210OcrUhs = ocr;
            SdhcExtension->T210OcrUhsCount++;
        }
    }
    /* From CMD11 until the voltage is reverted, record every command's R1 with
       its index: that window is where the port driver decides the card is not
       usable at 1.8 V, and the error bits there say why. */
    if (NT_SUCCESS(Status) && SdhcExtension->T210Enabled) {
        ULONG r1 = SdhcReadRegisterUlong(SdhcExtension, SDHC_RESPONSE);
        if (SdhcExtension->T210UhsWindow) {
            ULONG slot = SdhcExtension->T210R1Count & 15u;
            SdhcExtension->T210R1Idx[slot] = Command->Index & 0x3fu;
            SdhcExtension->T210R1Val[slot] = r1;
            SdhcExtension->T210R1Count = SdhcExtension->T210R1Count + 1u;
        }
        if (Command->Index == 3) SdhcExtension->T210Rca = (USHORT)(r1 >> 16);
        if (Command->Index == 6) SdhcExtension->T210Cmd6R1 = r1;
        else if (Command->Index == 7) SdhcExtension->T210Cmd7R1 = r1;
        else if (Command->Index == 13) SdhcExtension->T210Cmd13R1 = r1;
    }
#endif
#if NV_T210_LITE_WINARM_WRITE
    if (NT_SUCCESS(Status) && Command->Index == 41 && Command->ResponseType == SdResponseTypeR3) {
        if ((LiteU32((const unsigned char *)ResponseBuffer) & 0xc0000000u) == 0xc0000000u)
            InterlockedOr(&SdhcExtension->TargetFacts, LiteSdxc);
        else
            InterlockedAnd(&SdhcExtension->TargetFacts, ~LiteSdxc);
    }
#endif
    NT_ASSERT(NT_SUCCESS(Status));
}

VOID
SdhcRequestDpc(
    _In_ PVOID PrivateExtension,
    _Inout_ PSDPORT_REQUEST Request,
    _In_ ULONG Events,
    _In_ ULONG Errors
    )

/*++

Routine Description:

    DPC for interrupts associated with the given request.

Arguments:

    PrivateExtension - This driver's device extension (SdhcExtension).

    Request - Request operation to perform.

Return value:

    NTSTATUS - Return value of the request performed.

--*/

{

    ULONG Index;
    PSDHC_EXTENSION SdhcExtension;
    NTSTATUS Status;

    //
    // Find the index of the request in the outstanding request list.
    //

    SdhcExtension = (PSDHC_EXTENSION) PrivateExtension;
    for (Index = 0; Index < SDHC_MAX_OUTSTANDING_REQUESTS; Index += 1) {
        if (SdhcExtension->OutstandingRequests[Index] == Request) {
            break;
        }
    }

    // A late completion can arrive after its request left the outstanding list.
    if (Index == SDHC_MAX_OUTSTANDING_REQUESTS) {
        return;
    }
    Request->RequiredEvents &= ~Events;
#if NV_T214_SDR104
    if (Errors && SdhcExtension->T214DmaActive) {
        SdhcExtension->T214DmaErrors++;
        SdhcExtension->T214DmaFault = TRUE;
        SdhcExtension->T214DmaActive = FALSE;
        SdhcExtension->T214DmaComplete = FALSE;
        SdhcWriteRegisterUchar(SdhcExtension, SDHC_RESET, SDHC_RESET_DAT);
    } else if (SdhcExtension->T214DmaActive && Request->RequiredEvents == 0) {
        T214DmaDone(SdhcExtension, Request);
    }
#endif
#if NV_T210_UHS_DMA
    if (Errors && SdhcExtension->T210DmaActive) {
        SdhcExtension->T210DmaErrors++;
        SdhcExtension->T210DmaLastError = SdhcReadRegisterUshort(SdhcExtension, SDHC_ERROR_STATUS);
        SdhcExtension->T210DmaLastAdmaError = SdhcReadRegisterUchar(SdhcExtension, SDHC_ADMA_ERROR_STATUS);
        /* DAC off for the rest of this boot, but keep the disk alive: the
           request below is handed back on the plain PIO path instead of
           failing every transfer from now on. */
        SdhcExtension->T210DmaFault = TRUE;
        SdhcExtension->T210DmaActive = FALSE;
        SdhcExtension->T210DmaComplete = FALSE;
        SdhcWriteRegisterUchar(SdhcExtension, SDHC_RESET, SDHC_RESET_DAT);
    } else if (SdhcExtension->T210DmaActive && Request->RequiredEvents == 0) {
        T210DmaDone(SdhcExtension, Request);
    } else if ((Errors == 0) &&
               (Request->RequiredEvents == 0) &&
               (Request->Type == SdRequestTypeStartTransfer) &&
               (Request->Command.TransferDirection == SdTransferDirectionRead) &&
               (SdhcExtension->T210DmaTested == FALSE) &&
               (NvDiag.DataCommands >= NV_T210_DMA_PROBE_AFTER) &&
               /* Fires on the first large read after the boot gates: NTFS
                  read-ahead turns small requests into 32-block commands. */
               (NvDiag.DataStartBlocks >= NV_T210_DMA_PROBE_MIN_BLOCKS) &&
               (SdhcExtension->T210SlotStart != 0) &&
               (NvDiag.LastDataIt != 0) &&
               ((KeQueryInterruptTime() - NvDiag.LastDataIt) >=
                ((LONGLONG) NV_T210_DMA_PROBE_IDLE_SEC * 10000000LL)) &&
               ((KeQueryInterruptTime() - SdhcExtension->T210SlotStart) >=
                ((LONGLONG) NV_T210_DMA_PROBE_DELAY_SEC * 10000000LL))) {
        /* The last transfer just completed after a long quiet period: take the
           ADMA-vs-PIO verdict here, where the bus is idle by construction. The
           marker is flushed before any raw command, so a hang is visible. */
        SdhcExtension->T210DmaProbeCalls++;
        NvDiagCapture(L"UhsDmaProbeCalls", SdhcExtension->T210DmaProbeCalls);
        T210DmaProbe(SdhcExtension);
        NvDiagCapture(L"UhsDmaProbeSkip", SdhcExtension->T210DmaProbeSkip);
        if (SdhcExtension->T210DmaTestResult != 0) {
            NvDiagCapture(L"UhsDmaTest", SdhcExtension->T210DmaTestResult);
            NvDiagCapture(L"UhsDmaStDma", SdhcExtension->T210DmaTestStDma);
            NvDiagCapture(L"UhsDmaStPio", SdhcExtension->T210DmaTestStPio);
            NvDiagCapture(L"UhsDmaSig", SdhcExtension->T210DmaSig);
            NvDiagCapture(L"UhsDmaOk", SdhcExtension->T210DmaOk ? 1u : 0u);
        }
    }
#endif
#if NV_T210_LITE_WINARM_WRITE
    if (Errors) {
        SdhcExtension->AppPrefixAccepted = FALSE;
    } else if (Request->Type == SdRequestTypeCommandNoTransfer &&
               (Events & SDHC_IS_CMD_COMPLETE) != 0) {
        if (Request->Command.Index == 55) {
            const ULONG response = SdhcReadRegisterUlong(SdhcExtension, SDHC_RESPONSE);
            SdhcExtension->AppPrefixAccepted = (BOOLEAN)((response & 0x20u) != 0);
        } else if (Request->Command.Index == 42 && Request->Command.Class == SdCommandClassApp) {
            NvDiag.App42Done++;
        }
    }
#endif
    if (Errors) {
#if NV_T214_DDR200
        if (SdhcExtension->T214DdrActive) {
            SdhcExtension->T214DdrStatus = SdhcConvertErrorToStatus((USHORT)Errors);
            SdhcExtension->T214DdrRecoverNeeded = TRUE;
            SdhcExtension->T214DdrFatal = TRUE;
        }
#endif
        NvDiag.LastErrorCommand = Request->Command.Index;
        NvDiag.LastErrorKind = NvDiagErrorKind(Errors);
        NvDiag.LastErrors = Errors;
        NvDiag.CommandErrors += 1;
        if (Request->Command.TransferMethod == SdTransferMethodSgDma) {
            NvDiag.SgDmaErrors += 1;
            NvDiag.SgDmaErrStatus = Errors;
            NvDiag.SgDmaAdmaErr = SdhcReadRegisterUchar(SdhcExtension, SDHC_ADMA_ERROR_STATUS);
        }
    } else if ((Request->RequiredEvents == 0) &&
               (Request->Type != SdRequestTypeStartTransfer)) {
        NvDiag.CommandsOk += 1;
    }

    if ((Request == NvDiag.DataRequest) &&
        ((Errors != 0) ||
         ((Request->RequiredEvents == 0) &&
          (Request->Status != STATUS_MORE_PROCESSING_REQUIRED)))) {

        LARGE_INTEGER Frequency;

        NvDiag.DataEnd = KeQueryPerformanceCounter(&Frequency).QuadPart;
        if (Frequency.QuadPart != 0) {
            NvDiag.DataUs += (ULONG) ((NvDiag.DataEnd - NvDiag.DataStart) * 1000000 /
                                      Frequency.QuadPart);
        }

        NvDiag.DataRequest = NULL;
        NvDiag.DataCommands += 1;
        NvDiag.DataBlocks += NvDiag.DataStartBlocks;
        NvDiag.LastDataIt = KeQueryInterruptTime();
#if NV_DIAG_CLOCK
        if ((NvDiag.DataCommands % 1024) == 0) {
            NvDiagSavePeriodic(SdhcExtension);
        }
#endif
    }

#if NV_T210_LITE_WINARM_WRITE && NV_DIAG_CLOCK
    if ((Errors && Request->Command.Index != 5) ||
        (Request->Command.Index == 42 && NvDiag.App42Done == 1) ||
        NvDiag.SnapshotFacts != SdhcExtension->TargetFacts) {
        NvDiag.SnapshotFacts = SdhcExtension->TargetFacts;
        NvDiagSaveClock(SdhcExtension);
    }
#endif

    if (Errors) {
        Request->RequiredEvents = 0;
        SdhcExtension->OutstandingRequests[Index] = NULL;
        Status = SdhcConvertErrorToStatus((USHORT) Errors);
        SdPortCompleteRequest(Request, Status);

    } else if (Request->RequiredEvents == 0) {
#if NV_T214_DDR200
        if (Request->Status != STATUS_MORE_PROCESSING_REQUIRED)
            T214ObserveCardInfo(SdhcExtension, Request);
#endif
        if (Request->Status != STATUS_MORE_PROCESSING_REQUIRED) {
            Request->Status = STATUS_SUCCESS;
        }

        SdhcExtension->OutstandingRequests[Index] = NULL;
        SdPortCompleteRequest(Request, Request->Status);
    }
}

VOID
SdhcSlotToggleEvents(
    _In_ PVOID PrivateExtension,
    _In_ ULONG EventMask,
    _In_ BOOLEAN Enable
    )

/*++

Routine Description:

    Enable or disable the given event mask.

Arguments:

    SlotExtension - SlotExtension interface between sdhost and this miniport.

    Events - The event mask to toggle.

    Enable - TRUE to enable, FALSE to disable.

Return value:

    None.

--*/

{

    USHORT InterruptMask;
    PSDHC_EXTENSION SdhcExtension;

    InterruptMask = SdhcConvertEventsToHwMask(EventMask);
    SdhcExtension = (PSDHC_EXTENSION) PrivateExtension;
    if (Enable) {
        SdhcEnableInterrupt(SdhcExtension, 
                            InterruptMask);

    } else {
        SdhcDisableInterrupt(SdhcExtension, 
                             InterruptMask);
    }
}

VOID
SdhcSlotClearEvents(
    _In_ PVOID PrivateExtension,
    _In_ ULONG EventMask
    )

{

    USHORT Interrupts;
    PSDHC_EXTENSION SdhcExtension;

    SdhcExtension = (PSDHC_EXTENSION) PrivateExtension;
    Interrupts = SdhcConvertEventsToHwMask(EventMask);
    SdhcAcknowledgeInterrupts(SdhcExtension, Interrupts);
}

VOID
SdhcSaveContext(
    _In_ PVOID PrivateExtension
    )

/*++

Routine Description:

    Save slot register context.

Arguments:

    SlotExtension - SlotExtension interface between sdhost and this miniport.

Return value:

    None.

--*/

{
#if NV_T214_SDR104
    T214SaveContext((PSDHC_EXTENSION)PrivateExtension);
#else
    UNREFERENCED_PARAMETER(PrivateExtension);
#endif
}

VOID
SdhcRestoreContext(
    _In_ PVOID PrivateExtension
    )

/*++

Routine Description:

    Restore slot register context from a previously saved context.

Arguments:

    SlotExtension - SlotExtension interface between sdhost and this miniport.

Return value:

    None.

--*/

{
#if NV_T214_SDR104
    T214RestoreContext((PSDHC_EXTENSION)PrivateExtension);
#else
    UNREFERENCED_PARAMETER(PrivateExtension);
#endif
}

NTSTATUS
SdhcPoFxPowerControlCallback(
    _In_ PSD_MINIPORT Miniport,
    _In_ LPCGUID PowerControlCode,
    _In_reads_bytes_opt_(InputBufferSize) PVOID InputBuffer,
    _In_ SIZE_T InputBufferSize,
    _Out_writes_bytes_opt_(OutputBufferSize) PVOID OutputBuffer,
    _In_ SIZE_T OutputBufferSize,
    _Out_opt_ PSIZE_T BytesReturned
    )

/*++

Routine Description:

    Handle any PoFxPowerControl callbacks.

Arguments:

    Miniport - Miniport interface for the controller.

    PowerControlCode - GUID defining a platform-specific PoFxPowerControl
                       method.

    InputBuffer - Buffer containing any input arguments.

    InputBufferSize - Size of InputBuffer in bytes.

    OutputBuffer - Buffer containing any output results.

    OutputBufferSize - Size of OutputBuffer in bytes.

    BytesReturned - Number of bytes returned.

Return value:

    NTSTATUS

--*/

{

    UNREFERENCED_PARAMETER(Miniport);
    UNREFERENCED_PARAMETER(PowerControlCode);
    UNREFERENCED_PARAMETER(InputBuffer);
    UNREFERENCED_PARAMETER(InputBufferSize);
    UNREFERENCED_PARAMETER(OutputBuffer);
    UNREFERENCED_PARAMETER(OutputBufferSize);
    UNREFERENCED_PARAMETER(BytesReturned);

    return STATUS_NOT_IMPLEMENTED;
}

VOID
SdhcCleanup(
    _In_ PSD_MINIPORT Miniport
    )

/*++

Routine Description:

    Cleanup any memory allocations done during the lifetime of the driver.

Arguments:

    Miniport - Miniport interface for the controller.

Return value:

    NTSTATUS

--*/

{

    UNREFERENCED_PARAMETER(Miniport);
#if NV_T214_SDR104
    if (NvDiag.Extension != NULL) {
        T214Release((PSDHC_EXTENSION)NvDiag.Extension);
        NvDiag.Extension = NULL;
    }
#endif
}

//-----------------------------------------------------------------------------
// Host routine implementations.
//-----------------------------------------------------------------------------

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
SdhcResetHost(
    _In_ PSDHC_EXTENSION SdhcExtension,
    _In_ UCHAR ResetType
    )

/*++

Routine Description:

    Execute a soft reset to the socket specified.

Arguments:

    SdhcExtension - Host controller specific driver context.

    ResetType - Either full, CMD, or DAT reset.

Return value:

    STATUS_SUCCESS - Reset succeeded.

    STATUS_INVALID_PARAMETER - Invalid reset type chosen.

--*/

{

    UCHAR HostControl;
    UCHAR Reset;
    UCHAR Retries;
    UCHAR Mask;

#if NV_T214_DDR200
    if (ResetType == SdResetTypeAll && SdhcExtension->T214DdrActive) {
        return T214DdrRecover(SdhcExtension);
    }
#endif
#if NV_T210_LITE_WINARM_WRITE
    SdhcExtension->AppPrefixAccepted = FALSE;
#endif
    switch (ResetType) {
    case SdResetTypeAll:
#if NV_T210_LITE_WINARM_WRITE
        LiteForgetCard(SdhcExtension);
#endif
        Mask = SDHC_RESET_ALL;
        break;

    case SdResetTypeCmd:
        Mask = SDHC_RESET_CMD;
        break;

    case SdResetTypeDat:
        Mask = SDHC_RESET_DAT;
        break;

    default:
        return STATUS_INVALID_PARAMETER;
    }

    //
    // Reset the host controller.
    //

#if NV_T214_SDR104
    if (SdhcExtension->T214DmaFault) Mask |= SDHC_RESET_DAT;
#endif
    Retries = 100;
    SdhcWriteRegisterUchar(SdhcExtension, SDHC_RESET, Mask);
    do {
        Retries -= 1;
        if (Retries == 0) {
            return STATUS_IO_TIMEOUT;
        }

        Reset = SdhcReadRegisterUchar(SdhcExtension, SDHC_RESET);

        if ((Reset & Mask) != 0) {
            SdPortWait(1000);
        }

    } while ((Reset & Mask) != 0);

    //
    // Set the max HW timeout for bus operations.
    //

    SdhcWriteRegisterUchar(SdhcExtension,
                           SDHC_TIMEOUT_CONTROL,
                           SDHC_TC_MAX_DATA_TIMEOUT);

    //
    // Clear detection interrupt after reset, we will pick up the
    // status from the present state register
    //

    SdhcWriteRegisterUshort(SdhcExtension,
                            SDHC_INTERRUPT_STATUS,
                            0xFFFF);

    //
    // Initialize DMA if the controller supports it.
    //

    HostControl = SdhcReadRegisterUchar(SdhcExtension, SDHC_HOST_CONTROL);
    HostControl &= ~SDHC_HC_DMA_SELECT_MASK;
    if (SdhcExtension->Capabilities.Supported.ScatterGatherDma) {
        if (SdhcExtension->Capabilities.Supported.Address64Bit) {
            HostControl |= SDHC_HC_DMA_SELECT_ADMA64;

        } else {
            HostControl |= SDHC_HC_DMA_SELECT_ADMA32;
        }
    }

#if NV_T214_SDR104
    SdhcExtension->T214DmaActive = FALSE;
    SdhcExtension->T214DmaComplete = FALSE;
    SdhcExtension->T214DmaFault = FALSE;
    if (ResetType == SdResetTypeAll) T214VendorReset(SdhcExtension);
#endif
#if NV_T210_UHS
    /* A host reset drops HOST_CONTROL2 back to default, i.e. it also clears
       1.8 V signalling; note it so the trail shows where the voltage went. */
    T210TraceAdd(SdhcExtension, 4u);
#endif
    SdhcWriteRegisterUchar(SdhcExtension, SDHC_HOST_CONTROL, HostControl);
    return STATUS_SUCCESS;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
SdhcSetClock(
    _In_ PSDHC_EXTENSION SdhcExtension,
    _In_ ULONG Frequency
    )

/*++

Routine Description:

    Set the clock to a given frequency.

Arguments:

    SdhcExtension - Host controller specific driver context.

    Frequency - The target frequency.

Return value:

    STATUS_SUCCESS - The clock was successfuly set.

    STATUS_IO_TIMEOUT - The clock did not stabilize in time.

--*/

{

    ULONG ActualFrequency;
    USHORT ClockControl;
    ULONG Delay;
    USHORT Mask;
    UCHAR Retries;
    
#if NV_T214_DDR200
    if (SdhcExtension->T214DdrActive) {
        NTSTATUS ddr = T214DdrClock(SdhcExtension, Frequency);
#if NV_DIAG_CLOCK
        NvDiagSaveClock(SdhcExtension);
#endif
        return ddr;
    }
#endif
#if NV_T214_SDR104
    ULONG limit = (SdhcExtension->T214Voltage18 && SdhcExtension->SpeedMode == SdhcSpeedModeSDR104) ? 200000 : 50000;
    if (Frequency > limit) Frequency = limit;
#else
    if (NV_T210_LITE_SLOT && Frequency > 25000) Frequency = 25000;
#endif

#if NV_T210_UHS
    if (SdhcExtension->T210Enabled) T210TraceAdd(SdhcExtension, 6u);
#endif
    ClockControl = SdhcReadRegisterUshort(SdhcExtension, SDHC_CLOCK_CONTROL);
    ClockControl &= ~(SDHC_CC_CLOCK_ENABLE | SDHC_CC_INTERNAL_CLOCK_ENABLE);
    SdhcWriteRegisterUshort(SdhcExtension, SDHC_CLOCK_CONTROL, ClockControl);
    if (Frequency == 0) {
        NvDiag.ClockKhzRequested = NvDiag.ClockKhzActual = 0;
        NvDiag.RegClockControl = ClockControl;
        return STATUS_SUCCESS;
    }
    ClockControl = SdhcCalcClockFrequency(SdhcExtension,
                                          Frequency,
                                          &ActualFrequency);

    NvDiag.ClockKhzRequested = Frequency;
    NvDiag.ClockKhzActual = ActualFrequency;

    ClockControl |= SDHC_CC_INTERNAL_CLOCK_ENABLE;
    SdhcWriteRegisterUshort(SdhcExtension, SDHC_CLOCK_CONTROL, ClockControl);

    //
    // Now the frequency is set, delay a few times to wait for it to become
    // stable.
    //

    Retries = 100;
    Mask = SDHC_CC_CLOCK_STABLE;
    do {
        Retries -= 1;
        if (Retries == 0) {
            return STATUS_IO_TIMEOUT;
        }

        ClockControl = 
            SdhcReadRegisterUshort(SdhcExtension, SDHC_CLOCK_CONTROL);
        
        if ((ClockControl & Mask) == 0) {
            SdPortWait(1000);
        }

    } while ((ClockControl & Mask) == 0);

    //
    // Clock is now stable, now enable it.
    //

    ClockControl |= SDHC_CC_CLOCK_ENABLE;
    SdhcWriteRegisterUshort(SdhcExtension, SDHC_CLOCK_CONTROL, ClockControl);
    NvDiag.RegClockControl = ClockControl;

    //
    // Some hardware need more time here to stabilize, but minimize latency
    // for fixed eMMC devices during runtime Dx transitions.
    //

    Delay = (SdhcExtension->Removable) ? (10 * 1000) : 50;
    SdPortWait(Delay);
#if NV_DIAG_CLOCK
    NvDiagSaveClock(SdhcExtension);
#endif
    return STATUS_SUCCESS;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
SdhcSetVoltage(
    _In_ PSDHC_EXTENSION SdhcExtension,
    _In_ SDPORT_BUS_VOLTAGE Voltage
    )

/*++

Routine Description:

    Set the slot's voltage profile.

Arguments:

    SdhcExtension - Host controller specific driver context.

    VoltageProfile - Indicates which power voltage to use. If 0, turn off the
                     power

Return value:

    STATUS_SUCCESS - The bus voltage was successfully switched.

    STATUS_INVALID_PARAMETER - The voltage profile provided was invalid.

    STATUS_IO_TIMEOUT - The bus voltage did not stabilize in time.

--*/

{

    ULONG Delay;
    UCHAR Mask;
    UCHAR PowerControl;
    UCHAR PowerControlCheck;
    UCHAR Retries;

#if NV_T214_SDR104
    if (Voltage != SdBusVoltage33 && Voltage != SdBusVoltageOff) return STATUS_NOT_SUPPORTED;
    return T214SetBusVoltage(SdhcExtension, Voltage);
#endif
    //
    // Wait 10ms if the slot is removable; otherwise, only wait 50us.
    //

    Delay = (SdhcExtension->Removable) ? (10*  1000) : 50;
    SdPortWait(Delay);

    //
    // Use the highest voltage capable.
    //

    if (NV_T210_LITE_SLOT && Voltage != SdBusVoltage33 && Voltage != SdBusVoltageOff) {
        return STATUS_NOT_SUPPORTED;
    }

    switch (Voltage) {
    case SdBusVoltage33:
        PowerControl = SDHC_PC_3_3V;
        break;

    case SdBusVoltage30:
        PowerControl = SDHC_PC_3_0V;
        break;

    case SdBusVoltage18:
        PowerControl = SDHC_PC_1_8V;
        break;

    case SdBusVoltageOff:
#if NV_T210_LITE_WINARM_WRITE
        LiteForgetCard(SdhcExtension);
#endif
        SdhcWriteRegisterUchar(SdhcExtension, SDHC_POWER_CONTROL, 0);
        SdPortWait(Delay);
        PowerControlCheck = SdhcReadRegisterUchar(SdhcExtension, SDHC_POWER_CONTROL);
        return (PowerControlCheck & SDHC_PC_BUS_POWER) ?
               STATUS_DEVICE_POWER_FAILURE : STATUS_SUCCESS;

    default:

        NT_ASSERTMSG("SDHC - Voltage profile invalid.", FALSE);

        return STATUS_INVALID_PARAMETER;
    }

    Retries = 100;
    Mask = SDHC_PC_VOLTAGE_MASK;
    do {
        Retries -= 1;
        if (Retries == 0) {
            return STATUS_IO_TIMEOUT;
        }

        SdhcWriteRegisterUchar(SdhcExtension, SDHC_POWER_CONTROL, PowerControl);
        PowerControlCheck = 
            SdhcReadRegisterUchar(SdhcExtension, SDHC_POWER_CONTROL);
        
        if ((PowerControlCheck & Mask) != PowerControl) {
            SdPortWait(1000);
        }

    } while ((PowerControlCheck & Mask) != PowerControl);

    PowerControl |= SDHC_PC_BUS_POWER;
    SdhcWriteRegisterUchar(SdhcExtension, SDHC_POWER_CONTROL, PowerControl);
    SdPortWait(Delay);

    Retries = 100;
    Mask = SDHC_PC_VOLTAGE_MASK | SDHC_PC_BUS_POWER;
    do {
        Retries -= 1;
        if (Retries == 0) {
            return STATUS_IO_TIMEOUT;
        }

        SdhcWriteRegisterUchar(SdhcExtension, SDHC_POWER_CONTROL, PowerControl);
        PowerControlCheck = 
            SdhcReadRegisterUchar(SdhcExtension, SDHC_POWER_CONTROL);
        
        if ((PowerControlCheck & Mask) != PowerControl) {
            SdPortWait(1000);
        }

    } while ((PowerControlCheck & Mask) != PowerControl);

    return STATUS_SUCCESS;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
SdhcSetBusWidth(
    _In_ PSDHC_EXTENSION SdhcExtension,
    _In_ UCHAR Width
    )

/*++

Routine Description:

    Set bus width for host controller.

Arguments:

    SdhcExtension - Host controller specific driver context.

    Width - The data bus width of the slot.

Return value:

    STATUS_SUCCESS - The bus width was properly changed.

--*/

{

    UCHAR HostControl;

    if (NV_T210_LITE_SLOT && Width != 1 && Width != 4) {
        return STATUS_NOT_SUPPORTED;
    }

    HostControl = SdhcReadRegisterUchar(SdhcExtension, SDHC_HOST_CONTROL);
    HostControl &= ~(SDHC_HC_DATA_WIDTH_4BIT | SDHC_HC_DATA_WIDTH_8BIT);
    switch (Width) {
    case 1:
        break;

    case 4:
        HostControl |= SDHC_HC_DATA_WIDTH_4BIT;
        break;
    case 8:
        HostControl |= SDHC_HC_DATA_WIDTH_8BIT;
        break;

    default:

        NT_ASSERTMSG("SDHC - Provided bus width is invalid", FALSE);

        break;
    }

    SdhcWriteRegisterUchar(SdhcExtension, SDHC_HOST_CONTROL, HostControl);
    return STATUS_SUCCESS;
}

NTSTATUS
SdhcSetSpeed(
    _In_ PSDHC_EXTENSION SdhcExtension,
    _In_ SDPORT_BUS_SPEED Speed
    )

/*++

Routine Description:

    Based on the capabilities of card and host, turns on the maximum performing
    speed mode for the host. Caller is expected to know the capabilities of the
    card before setting the speed mode.

Arguments:

    SdhcExtension - Host controller specific driver context.

    Speed - Bus speed to set.

Return value:

    STATUS_SUCCESS - The selected speed mode was successfully set.

    STATUS_INVALID_PARAMETER - The speed mode selected is not valid.

--*/

{

    NTSTATUS Status;
    USHORT UhsMode;

#if NV_T214_DDR200
    if (SdhcExtension->T214DdrActive)
        return Speed == SdBusSpeedSDR104 ? STATUS_SUCCESS : STATUS_NOT_SUPPORTED;
#endif
    if (NV_T210_LITE_SLOT && !NV_T214_SDR104 && Speed != SdBusSpeedNormal) {
        return STATUS_NOT_SUPPORTED;
    }

#if NV_T210_UHS
    /* What SDPORT asked for, for the diag trail (7 = SDR104 in sdport.h). */
    SdhcExtension->T210SpeedSeen = (ULONG)Speed;
    if (Speed == SdBusSpeedSDR104) T210TraceAdd(SdhcExtension, 3u);
    else if (Speed == SdBusSpeedSDR25 || Speed == SdBusSpeedSDR12) T210TraceAdd(SdhcExtension, 7u);
    else if (Speed == SdBusSpeedSDR50) T210TraceAdd(SdhcExtension, 8u);
    else if (Speed == SdBusSpeedDDR50) T210TraceAdd(SdhcExtension, 9u);
#endif

#if NV_T214_SDR104
    if (Speed == SdBusSpeedSDR50 || Speed == SdBusSpeedDDR50 || Speed == SdBusSpeedHS200 || Speed == SdBusSpeedHS400)
        return STATUS_NOT_SUPPORTED;
#endif
    /* Applies to both profiles: the T210 tuning path keys off SpeedMode too, and
       leaving the assignment inside the T214 guard kept it Normal forever. */
    SdhcExtension->SpeedMode =
        Speed == SdBusSpeedSDR104 ? SdhcSpeedModeSDR104 :
        Speed == SdBusSpeedSDR50 ? SdhcSpeedModeSDR50 :
        Speed == SdBusSpeedDDR50 ? SdhcSpeedModeDDR50 :
        (Speed == SdBusSpeedHigh || Speed == SdBusSpeedSDR25) ? SdhcSpeedModeHigh :
        SdhcSpeedModeNormal;
    switch (Speed) {
    case SdBusSpeedNormal:
        Status = SdhcSetHighSpeed(SdhcExtension, FALSE);
        break;

    case SdBusSpeedHigh:
        Status = SdhcSetHighSpeed(SdhcExtension, TRUE);
        break;

    case SdBusSpeedSDR12:
    case SdBusSpeedSDR25:
    case SdBusSpeedSDR50:
    case SdBusSpeedDDR50:
    case SdBusSpeedSDR104:
    case SdBusSpeedHS200:
    case SdBusSpeedHS400:
        UhsMode = SdhcGetHwUhsMode(Speed);
        Status = SdhcSetUhsMode(SdhcExtension, UhsMode);
#if NV_T210_UHS && !defined(NV_T210_UHS_NO_CAR)
        if (NT_SUCCESS(Status) && Speed == SdBusSpeedSDR104 &&
            SdhcExtension->T210Enabled && SdhcExtension->T210Sdr104) {
            /* T210 (Erista): SDR104 needs the fast parent clock and a tuning
               pass. Any failure drops back to 3.3 V high speed, which is the
               only mode this console is known to run. */
            NTSTATUS uhs = T210ClockSdr104(SdhcExtension);
            NvDiagCapture(L"UhsClockStatus", (ULONG)uhs);
            if (NT_SUCCESS(uhs)) uhs = T210Tune(SdhcExtension);
            NvDiagCapture(L"UhsTuneStatus2", (ULONG)uhs);
            NvDiagCapture(L"UhsBaseKhz", SdhcExtension->T210BaseKhz);
            NvDiagCapture(L"UhsTap", SdhcExtension->T210Tap);
            if (!NT_SUCCESS(uhs)) {
                (VOID)T210Power3V3(SdhcExtension);
                Status = uhs;
            }
        }
#endif
#if defined(NV_T210_UHS_NO_CAR) && NV_T210_UHS
        /* Safety profile: keep the CAR at the verified 48 MHz PLLP divider
           (no PLLC4 touch, the GPU clock domain stays untouched). SDR104 mode
           is still selected above via HOST_CONTROL2; SDPORT clocks it from
           the advertised 48 MHz base, so the card runs the UHS-I signalling
           at 48 MHz without needing a tuning pass. */
        if (NT_SUCCESS(Status) && Speed == SdBusSpeedSDR104 &&
            SdhcExtension->T210Enabled && SdhcExtension->T210Sdr104) {
            NvDiagCapture(L"UhsSdr104Mode", 1);
#if NV_T210_UHS_TUNE_AT_SPEED
            /* Tune here, while the rail is still 1.8 V. SDPORT only asks for
               tuning after it has already run a check transfer, so an untuned
               sampling point is exactly what makes that check fail and sends
               the port driver back to 3.3 V (trace 1,3,2). */
            if (SdhcExtension->T210Voltage18 && !SdhcExtension->T210Tuned) {
                NTSTATUS atSpeed = T210Tune(SdhcExtension);
                NvDiagCapture(L"UhsTuneAtSpeed", (ULONG)atSpeed);
                NvDiagCapture(L"UhsTuned", SdhcExtension->T210Tuned ? 1u : 0u);
                NvDiagCapture(L"UhsTap", SdhcExtension->T210Tap);
            }
#endif
#if NV_T210_UHS_POSTCHECK2
            /* Bring-up: by now SDPORT has programmed the SDR104 timing, so a
               CMD6 query is answered only if the 1.8 V link really works.
               Reporting: 0xDA08 = answered, 0xDA09 = not answered. */
            T210PostCheck(SdhcExtension);
#endif
        }
#endif
        break;

    default:

        NT_ASSERTMSG("SDHC - Invalid speed mode selected.", FALSE);

        Status = STATUS_INVALID_PARAMETER;
        break;
    }

    return Status;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
SdhcSetHighSpeed(
    _In_ PSDHC_EXTENSION SdhcExtension,
    _In_ BOOLEAN Enable
    )

/*++

Routine Description:

    Based on the capabilities of card and host, enables or disables high speed.

Arguments:

    SdhcExtension - Host controller specific driver context.

    Enable - TRUE to enable high speed, FALSE to disable.

Return value:

    STATUS_SUCCESS - High speed was successfully enabled.

--*/

{

    UCHAR HostControl;

    HostControl = SdhcReadRegisterUchar(SdhcExtension, SDHC_HOST_CONTROL);
    HostControl &= ~SDHC_HC_ENABLE_HIGH_SPEED;
    if (Enable && !NV_T214_SDR104) {
        HostControl |= SDHC_HC_ENABLE_HIGH_SPEED;
    }

    SdhcWriteRegisterUchar(SdhcExtension, SDHC_HOST_CONTROL, HostControl);
    return STATUS_SUCCESS;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
SdhcSetUhsMode(
    _In_ PSDHC_EXTENSION SdhcExtension,
    _In_ USHORT Mode
    )

/*++

Routine Description:

    Based on the capabilities of card and host, turns on the maximum performing
    speed mode for the host. Caller is expected to know the capabilities of the
    card before setting the speed mode.

Arguments:

    SdhcExtension - Host controller specific driver context.

    Mode - UHS mode to set.

Return value:

    STATUS_SUCCESS - Mode requested is set on the controller.

    STATUS_INVALID_PARAMETER - The mode selected is not a UHS mode.

--*/

{
    
    USHORT ClockControl;
    USHORT HostControl2;

    HostControl2 = SdhcReadRegisterUshort(SdhcExtension, SDHC_HOST_CONTROL2);

    //
    // If we're already in the requested mode, return.
    //

    if ((HostControl2 & SDHC_HC2_UHS_MODES) == Mode) {
        return STATUS_SUCCESS;
    }

    ClockControl = SdhcReadRegisterUshort(SdhcExtension, SDHC_CLOCK_CONTROL);
    ClockControl &= ~SDHC_CC_CLOCK_ENABLE;
    SdhcWriteRegisterUshort(SdhcExtension, SDHC_CLOCK_CONTROL, ClockControl);
    SdPortWait(10 * 1000);

    //
    // Set the UHS mode.
    //

    HostControl2 &= ~SDHC_HC2_UHS_MODES;
    HostControl2 |= Mode;
    SdhcWriteRegisterUshort(SdhcExtension, SDHC_HOST_CONTROL2, HostControl2);
    ClockControl = SdhcReadRegisterUshort(SdhcExtension, SDHC_CLOCK_CONTROL);
    ClockControl |= SDHC_CC_CLOCK_ENABLE;
    SdhcWriteRegisterUshort(SdhcExtension, SDHC_CLOCK_CONTROL, ClockControl);
    SdPortWait(10 * 1000);
    return STATUS_SUCCESS;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
SdhcSetSignaling(
    _In_ PSDHC_EXTENSION SdhcExtension,
    _In_ BOOLEAN Enable
    )

/*++

Routine Description:

    Set signaling voltage (1.8V or 3.3V).

Arguments:

    SdhcExtension - Host controller specific driver context.

    Enable - TRUE for 1.8V signaling, FALSE for default 3.3V.

Return value:

    STATUS_SUCCESS - Signaling voltage switch successful.

    STATUS_UNSUCCESSFUL - Signaling voltage switch unsuccessful.

--*/

{

    USHORT ClockControl;
    ULONG DatLines;
    USHORT HostControl2;
    USHORT Mask;

#if NV_T214_SDR104
    NTSTATUS t214Status = T214Signaling(SdhcExtension, Enable);
#if NV_DIAG_CLOCK
    NvDiagSaveClock(SdhcExtension);
#endif
    return t214Status;
#endif
#if NV_T210_UHS
    if (SdhcExtension->T210Enabled) {
        NTSTATUS t210Status;
#if NV_T210_UHS_HOLD_18V
        if (!Enable && SdhcExtension->T210Voltage18 && SdhcExtension->T210SigOff == 0) {
            /* First 3.3 V request right after a successful switch: keep the rail
               at 1.8 V (card and host still agree), to see whether SDPORT only
               wanted to undo the switch. */
            SdhcExtension->T210SigOff++;
            SdhcExtension->T210Hold18V++;
            T210TraceAdd(SdhcExtension, 2u);
            return STATUS_SUCCESS;
        }
#endif
        if (Enable) {
            SdhcExtension->T210SigOn++;
            SdhcExtension->T210UhsWindow = TRUE;
            SdhcExtension->T210R1Count = 0;
            SdhcExtension->T210WinStartLo = (ULONG)KeQueryInterruptTime();
        } else {
            SdhcExtension->T210SigOff++;
            SdhcExtension->T210WinMs =
                (ULONG)((KeQueryInterruptTime() - (ULONGLONG)SdhcExtension->T210WinStartLo) / 10000u);
            SdhcExtension->T210SnapPs = SdhcReadRegisterUlong(SdhcExtension, SDHC_PRESENT_STATE);
            SdhcExtension->T210SnapHc2 = SdhcReadRegisterUshort(SdhcExtension, SDHC_HOST_CONTROL2);
            SdhcExtension->T210SnapPwr = SdhcReadRegisterUchar(SdhcExtension, SDHC_POWER_CONTROL);
            SdhcExtension->T210SnapClk = SdhcReadRegisterUshort(SdhcExtension, SDHC_CLOCK_CONTROL);
            SdhcExtension->T210SnapIs = SdhcReadRegisterUshort(SdhcExtension, SDHC_INTERRUPT_STATUS);
            SdhcExtension->T210SnapErr = SdhcReadRegisterUshort(SdhcExtension, SDHC_ERROR_STATUS);
            SdhcExtension->T210UhsWindow = FALSE;
        }
        T210TraceAdd(SdhcExtension, Enable ? 1u : 2u);
        t210Status = T210Signaling(SdhcExtension, Enable);
        SdhcExtension->T210SigStatus = (ULONG)t210Status;
#if NV_DIAG_CLOCK
        NvDiagSaveClock(SdhcExtension);
#endif
        return t210Status;
    }
#endif
    if (NV_T210_LITE_SLOT && Enable) {
        return STATUS_NOT_SUPPORTED;
    }

    // SDPORT can request default signaling again after the card is selected.
    // With the bus idle DAT[3:0] are high; no CMD11 handshake is in progress.
    HostControl2 = SdhcReadRegisterUshort(SdhcExtension, SDHC_HOST_CONTROL2);
    if (!Enable && !(HostControl2 & SDHC_HC2_1_8V_SIGNALING)) {
        return STATUS_SUCCESS;
    }
    if (NV_T210_LITE_SLOT) {
        // The firmware handoff and this SDMMC1 profile both require 3.3 V.
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    //
    // Disable the bus clock.
    //

    ClockControl = SdhcReadRegisterUshort(SdhcExtension, SDHC_CLOCK_CONTROL);
    ClockControl &= ~SDHC_CC_CLOCK_ENABLE;
    SdhcWriteRegisterUshort(SdhcExtension, SDHC_CLOCK_CONTROL, ClockControl);
    SdPortWait(10000);

    //
    // DAT[3:0] must be all zeroes.
    //

    DatLines = 0;
    DatLines = 
        SDHC_PS_DAT_3_0 &
        SdhcReadRegisterUlong(SdhcExtension, SDHC_PRESENT_STATE);

    if (DatLines != 0) {
        return STATUS_UNSUCCESSFUL;
    }

    //
    // Set the signaling voltage.
    //

    HostControl2 = SdhcReadRegisterUshort(SdhcExtension, SDHC_HOST_CONTROL2);
    Mask = SDHC_HC2_1_8V_SIGNALING;
    if (Enable) {
        HostControl2 |= Mask;

    } else {
        HostControl2 &= ~Mask;
    }

    SdhcWriteRegisterUshort(SdhcExtension, SDHC_HOST_CONTROL2, HostControl2);
    SdPortWait(5000);
    HostControl2 = SdhcReadRegisterUshort(SdhcExtension, SDHC_HOST_CONTROL2);

    //
    // Make sure the signaling voltage latched.
    //

    if (Enable) {
        if ((HostControl2 & Mask) == 0) {
            return STATUS_UNSUCCESSFUL;
        }
        
    } else {
        if ((HostControl2 & Mask) != 0) {
            return STATUS_UNSUCCESSFUL;
        }
    }

    //
    // Reenable the bus clock.
    //

    ClockControl = SdhcReadRegisterUshort(SdhcExtension, SDHC_CLOCK_CONTROL);
    ClockControl |= SDHC_CC_CLOCK_ENABLE;
    SdhcWriteRegisterUshort(SdhcExtension, SDHC_CLOCK_CONTROL, ClockControl);
    SdPortWait(10000);

    //
    // DAT[3:0] must be all ones.
    //

    DatLines = 0;
    DatLines =
        SDHC_PS_DAT_3_0 &
        SdhcReadRegisterUlong(SdhcExtension, SDHC_PRESENT_STATE);

    if (DatLines != SDHC_PS_DAT_3_0) {
        return STATUS_UNSUCCESSFUL;
    }

    return STATUS_SUCCESS;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
SdhcExecuteTuning(
    _In_ PSDHC_EXTENSION SdhcExtension
    )

/*++

Routine Description:

    Tune the bus sampling point due to variations in voltage,
    temperature, and time.

    Caller guarantees that the bus is in a UHS mode that
    requires tuning.

Arguments:

    SdhcExtension - Host controller specific driver context.

Return value:

    STATUS_SUCCESS - Tuning successful.

--*/

{

    USHORT HostControl2;
    //UCHAR Retries;
    SDPORT_REQUEST TuningRequest;

#if NV_T214_SDR104
#if NV_T214_DDR200
    NTSTATUS t214Status = T214DdrTry(SdhcExtension);
#else
    NTSTATUS t214Status = T214Tune(SdhcExtension);
#endif
#if NV_DIAG_CLOCK
    NvDiagSaveClock(SdhcExtension);
#endif
    return t214Status;
#endif
#if NV_T210_UHS
    /* T210 (Erista): the generic SDHCI path below is a stub that only sets
       HC2.EXECUTE_TUNING and returns success, which leaves the controller in
       "tuning in progress" and never picks a sampling tap. Use the vendor
       tuning step registers instead (T210Tune, ported from T214Tune). */
    if (SdhcExtension->T210Enabled && SdhcExtension->T210Voltage18 &&
        SdhcExtension->SpeedMode == SdhcSpeedModeSDR104) {
        NTSTATUS t210Tune;
        T210TraceAdd(SdhcExtension, 5u);          /* tuning reached at SDR104 */
        t210Tune = T210Tune(SdhcExtension);
#if NV_DIAG_CLOCK
        NvDiagCapture(L"UhsTuneStatus", (ULONG)t210Tune);
        NvDiagCapture(L"UhsTuned", SdhcExtension->T210Tuned ? 1u : 0u);
        NvDiagCapture(L"UhsTap", SdhcExtension->T210Tap);
        NvDiagSaveClock(SdhcExtension);
#endif
#if NV_T210_UHS_BEACON && !NV_T214_SDR104
        /* 0xDA50 = vendor tuning succeeded, 0xDA51 = it failed. */
        KeBugCheckEx(NT_SUCCESS(t210Tune) ? 0xDA50u : 0xDA51u,
                     SdhcExtension->T210Tuned ? 1u : 0u,
                     SdhcExtension->T210Tap,
                     (ULONG)SdhcExtension->T210TuneIterations,
                     (ULONG)SdhcReadRegisterUshort(SdhcExtension, SDHC_HOST_CONTROL2));
#endif
        return t210Tune;
    }
    if (SdhcExtension->T210Enabled &&
        !(SdhcExtension->T210Voltage18 &&
          SdhcExtension->SpeedMode == SdhcSpeedModeSDR104)) {
        /* Tuning is only meaningful in 1.8 V SDR104 here. Success without any
           hardware access: the stock stub below would set EXECUTE_TUNING and
           leave it set, so the controller keeps "tuning" during the transfers
           that follow. */
        return STATUS_SUCCESS;
    }
#endif
    HostControl2 = SdhcReadRegisterUshort(SdhcExtension, SDHC_HOST_CONTROL2);

    NT_ASSERT((HostControl2 & SDHC_HC2_EXECUTE_TUNING) == 0);

    //
    // Disable controller events
    //
    // Technically, all controller events should be disabled at tuning execute
    // time, but some controllers do not follow this requirement.
    //

    if ((HostControl2 & SDHC_HC2_EXECUTE_TUNING) == 0) {
        HostControl2 |= SDHC_HC2_EXECUTE_TUNING;
        SdhcWriteRegisterUshort(SdhcExtension,
                                SDHC_HOST_CONTROL2,
                                HostControl2);
    }

    RtlZeroMemory(&TuningRequest, sizeof(TuningRequest));
    TuningRequest.Command.TransferType = SdTransferTypeSingleBlock;
    TuningRequest.Command.TransferDirection = SdTransferDirectionRead;
    TuningRequest.Command.Class = SdCommandClassStandard;
    TuningRequest.Command.ResponseType = SdResponseTypeR1;
    if (SdhcExtension->SpeedMode == SdhcSpeedModeSDR104) {
        TuningRequest.Command.Index = 19;
        TuningRequest.Command.BlockSize = 64;

    } else {
        TuningRequest.Command.Index = 21;
        TuningRequest.Command.BlockSize = 128;
    }

    //
    // TODO: Implement rest of tuning logic as described in
    // SD Host Controller Specification. This is needed for
    // SDR104, HS200, and HS400 modes, and sometimes for SDR50.
    //

    //
    // Execute the data transfer.
    //

    //Status = SdhcBuildTransfer()

    return STATUS_SUCCESS;
}

VOID
SdhcSetLed(
    _In_ PSDHC_EXTENSION SdhcExtension,
    _In_ BOOLEAN Enable
    )

/*++

Routine Description:

    Turn the controller activity LED on/off.

Arguments:

    SdhcExtension - Host controller specific driver context.

    Enable - Indicate whether to enable or disable the LED.

Return value:

    None.

--*/

{

    UCHAR HostControl;

    HostControl = SdhcReadRegisterUchar(SdhcExtension, SDHC_HOST_CONTROL);
    if (Enable) {
        HostControl |= SDHC_HC_LED_POWER;

    } else {
        HostControl &= ~SDHC_HC_LED_POWER;
    }

    SdhcWriteRegisterUchar(SdhcExtension, SDHC_HOST_CONTROL, HostControl);
}

NTSTATUS
SdhcSetPresetValue(
    _In_ PSDHC_EXTENSION SdhcExtension,
    _In_ BOOLEAN Enable
    )

/*++

Routine Description:

    Enable or disable setting of preset values. Caller must
    ensure that the controller supports preset value.

Arguments:

    SdhcExtension - Host controller specific driver context.

    Enable - Indicate whether to enable or disable preset values.

Return value:

    None.

--*/

{

    USHORT HostControl2;

    HostControl2 = SdhcReadRegisterUshort(SdhcExtension, SDHC_HOST_CONTROL2);

    NT_ASSERT((HostControl2 & SDHC_HC2_ENABLE_PRESET_VALUE) != 0);

    HostControl2 &= ~SDHC_HC2_ENABLE_PRESET_VALUE;
    if (Enable && !NV_T214_SDR104) {
        HostControl2 |= SDHC_HC2_ENABLE_PRESET_VALUE;
    }

    SdhcWriteRegisterUshort(SdhcExtension, SDHC_HOST_CONTROL2, HostControl2);
    return STATUS_SUCCESS;
}

NTSTATUS
SdhcEnableBlockGapInterrupt(
    _In_ PSDHC_EXTENSION SdhcExtension,
    _In_ BOOLEAN Enable
    )

/*++

Routine Description:

    Enables block gap interrupts for SDIO cards in 4-bit mode.
    Caller has responsibility to make sure this is only called for
    appropriate devices.

Arguments:

    SdhcExtension - Host controller specific driver context.

    Enable - Enable or disable block gap interrupts.

Return value:

    None.

--*/

{

    UCHAR Control;

    Control = SdhcReadRegisterUchar(SdhcExtension, SDHC_BLOCKGAP_CONTROL);
    if (Enable) {
        Control |= SDHC_BGC_INTERRUPT_ENABLE;

    } else {
        Control &= ~SDHC_BGC_INTERRUPT_ENABLE;
    }

    SdhcWriteRegisterUchar(SdhcExtension, SDHC_BLOCKGAP_CONTROL, Control);
    return STATUS_SUCCESS;
}

VOID
SdhcSetBlockGapControl(
    _In_ PSDHC_EXTENSION SdhcExtension,
    _In_ BOOLEAN Continue,
    _In_ BOOLEAN RequestStop
    )

/*++

Routine Description:

    Enable block gap interrupt requests.

Arguments:

    SdhcExtension - Host controller specific driver context.

    Continue - Continue after the next block gap.

    RequestStop - Request the block gap interrupt request.

Return value:

    None.

--*/

{

    UCHAR BlockGapControl;

    BlockGapControl = SdhcReadRegisterUchar(SdhcExtension,
                                            SDHC_BLOCKGAP_CONTROL);

    BlockGapControl &= ~SDHC_BGC_CONTINUE;
    BlockGapControl &= ~SDHC_BGC_STOP_NEXT_GAP;  
    if (Continue) {
        BlockGapControl |= SDHC_BGC_CONTINUE;
    }

    if (RequestStop) {
        BlockGapControl |= SDHC_BGC_STOP_NEXT_GAP;
    }
}

VOID
SdhcEnableInterrupt(
    _In_ PSDHC_EXTENSION SdhcExtension,
    _In_ ULONG NormalInterruptMask
    )

/*++

Routine Description:

    Set the host event mask to the new value specified.

Arguments:

    SdhcExtension - Host controller specific driver context.

    NormalInterruptMask - The new normal events to set.

Return value:

    None.

--*/

{

    USHORT InterruptEnable;

    InterruptEnable =
        SdhcReadRegisterUshort(SdhcExtension, SDHC_INTERRUPT_SIGNAL_ENABLE);

    InterruptEnable |= NormalInterruptMask;

    //
    // Enable the interrupt signals from controller to OS.
    //

    if (!SdhcExtension->CrashdumpMode) {
        SdhcWriteRegisterUshort(SdhcExtension,
                                SDHC_INTERRUPT_SIGNAL_ENABLE,
                                InterruptEnable);

        SdhcWriteRegisterUshort(SdhcExtension,
                                SDHC_ERROR_SIGNAL_ENABLE,
                                0xFFFF);
    }

    //
    // Enable the interrupt signals on the controller.
    //

    SdhcWriteRegisterUshort(SdhcExtension,
                            SDHC_INTERRUPT_STATUS_ENABLE,
                            InterruptEnable);

    SdhcWriteRegisterUshort(SdhcExtension,
                            SDHC_ERROR_STATUS_ENABLE,
                            0xFFFF);
}

VOID
SdhcDisableInterrupt(
    _In_ PSDHC_EXTENSION SdhcExtension,
    _In_ ULONG NormalInterruptMask
    )

/*++

Routine Description:

    Set the host event mask to the new value specified.

Arguments:

    SdhcExtension - Host controller specific driver context.

    NormalInterruptMask - Normal Interrupts to disable.

Return value:

    None.

--*/

{

    USHORT InterruptDisable;

    InterruptDisable =
        SdhcReadRegisterUshort(SdhcExtension, SDHC_INTERRUPT_SIGNAL_ENABLE);

    InterruptDisable &= ~NormalInterruptMask;

    //
    // Disable the interrupt signals on the controller.
    //
    
    SdhcWriteRegisterUshort(SdhcExtension,
                            SDHC_INTERRUPT_STATUS_ENABLE,
                            InterruptDisable);

    SdhcWriteRegisterUshort(SdhcExtension,
                            SDHC_ERROR_STATUS_ENABLE,
                            0);

    //
    // Disable the interrupt signals from controller to OS.
    //

    SdhcWriteRegisterUshort(SdhcExtension,
                            SDHC_INTERRUPT_SIGNAL_ENABLE,
                            InterruptDisable);

    SdhcWriteRegisterUshort(SdhcExtension,
                            SDHC_ERROR_SIGNAL_ENABLE,
                            0);
}

__forceinline
USHORT
SdhcGetInterruptStatus(
    _In_ PSDHC_EXTENSION SdhcExtension
    )

/*++

Routine Description:

    Get current pending events from the interrupt status. This function does
    not acknowledge the interrupt. The caller should acknowledge or disable
    the corresponding events.

Arguments:

    SdhcExtension - Host controller specific driver context.

Return value:

    The event mask.

--*/

{

    USHORT InterruptStatus;

    InterruptStatus = 
        SdhcReadRegisterUshort(SdhcExtension, SDHC_INTERRUPT_STATUS);

    //
    // 0xFFFF means HC is no longer accessible. This interrupt does not belong
    // to us.
    //

    if (InterruptStatus == 0xFFFF) {
        return 0;
    }

    return InterruptStatus;
}

__forceinline
USHORT
SdhcGetErrorStatus(
    _In_ PSDHC_EXTENSION SdhcExtension
    )

/*++

Routine Description:

    This routine returns the current error status, if any.

Arguments:

    SdhcExtension - Host controller specific driver context.

Return value:

    The error interrupt status.

--*/

{

    return SdhcReadRegisterUshort(SdhcExtension, SDHC_ERROR_STATUS);
}

__forceinline
USHORT
SdhcGetAutoCmd12ErrorStatus(
    _In_ PSDHC_EXTENSION SdhcExtension
    )

/*++

Routine Description:

    This routine returns the current Auto CMD12 error status, if any.

Arguments:

    SdhcExtension - Host controller specific driver context.

Return value:

    The Auto CMD12 error status.

--*/

{

    return SdhcReadRegisterUshort(SdhcExtension, SDHC_AUTO_CMD12_ERROR_STATUS);
}

USHORT
SdhcGetAdmaErrorStatus(
    _In_ PSDHC_EXTENSION SdhcExtension
    )

/*++

Routine Description:

    This routine returns the current ADMA error status, if any.

Arguments:

    SdhcExtension - Host controller specific driver context.

Return value:

    The ADMA error status.

--*/

{

    return SdhcReadRegisterUshort(SdhcExtension, SDHC_ADMA_ERROR_STATUS);
}

VOID
SdhcAcknowledgeInterrupts(
    _In_ PSDHC_EXTENSION SdhcExtension,
    _In_ USHORT Interrupts
    )

/*++

Routine Description:

    Acknowlege the interrupts specified.

Arguments:

    SdhcExtension - Host controller specific driver context.

    Interrupts - event mask to acknowledge (single event only)

Return value:

    None.

--*/

{

    if ((Interrupts & SDHC_IS_ERROR_INTERRUPT) != 0) {

        //
        // The Auto CMD12 error interrupt status bit of some Ricoh controllers
        // can't get cleared by writing to the error status register alone.
        // Write all-ones and all-zeroes to the Auto CMD12 error status register
        // first to work around this issue. This write should have no effect on
        // other types of controllers since the register should be read-only
        // according to the spec.
        //

        SdhcWriteRegisterUshort(SdhcExtension, 
                                SDHC_AUTO_CMD12_ERROR_STATUS,
                                0xFFFF);

        SdhcWriteRegisterUshort(SdhcExtension,
                                SDHC_AUTO_CMD12_ERROR_STATUS,
                                0x0);

        //
        // Clear the error interrupt by writing all-ones.
        //

        SdhcWriteRegisterUshort(SdhcExtension, SDHC_ERROR_STATUS, 0xFFFF);
        Interrupts &= ~SDHC_IS_ERROR_INTERRUPT;
    }

    //
    // Clear other interrupts in the interrupt status register.
    //

    SdhcWriteRegisterUshort(SdhcExtension, SDHC_INTERRUPT_STATUS, Interrupts);
}

BOOLEAN
SdhcIsCardInserted(
    _In_ PSDHC_EXTENSION SdhcExtension
    )

/*++

Routine Description:

    To detect whether there is a card in the socket.

Arguments:

    SdhcExtension - Host controller specific driver context.

Return value:

    BOOLEAN value to indicate whether there is a card in the socket.

--*/

{

    ULONG PresentState;

    PresentState = SdhcReadRegisterUlong(SdhcExtension, SDHC_PRESENT_STATE);
    return (BOOLEAN) ((PresentState & SDHC_PS_CARD_INSERTED) != 0);
}

BOOLEAN
SdhcIsWriteProtected(
    _In_ PSDHC_EXTENSION SdhcExtension
    )

/*++

Routine Description:

    To detect whether the card is write protected.

Arguments:

    SdhcExtension - Host controller specific driver context.

Return value:

    BOOLEAN value to indicate whether the card is write protected.

--*/

{

    ULONG PresentState;
    BOOLEAN WriteProtected;

    //
    // Write protect is active low.
    //

    PresentState = SdhcReadRegisterUlong(SdhcExtension, SDHC_PRESENT_STATE);
    WriteProtected = (BOOLEAN) ((PresentState & SDHC_PS_WRITE_PROTECT) == 0);
    return WriteProtected;
}

NTSTATUS
SdhcSendCommand(
    _In_ PSDHC_EXTENSION SdhcExtension,
    _In_ PSDPORT_REQUEST Request
    )

/*++

Routine Description:

    This routine takes the SD command package and writes it to the appropriate
    registers on the host controller. It also computes the proper flag
    settings.

Arguments:

    SdhcExtension - Host controller specific driver context.

    Request - Supplies the descriptor for this SD command

Return value:

    STATUS_SUCCESS - Command successfully sent.

    STATUS_INVALID_PARAMETER - Invalid command response type specified.

--*/

{

    PSDPORT_COMMAND Command;
    USHORT CommandType;
    USHORT CommandReg;
    NTSTATUS Status;
    USHORT TransferMode;

    //
    // Initialize transfer parameters if this command is a data command.
    //

    Command = &Request->Command;
    if ((Command->TransferType != SdTransferTypeNone) &&
        (Command->TransferType != SdTransferTypeUndefined)) {

        Status = SdhcBuildTransfer(SdhcExtension, Request);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
    }

    //
    // Set the response parameters based off the given response type.
    //

    SdhcWriteRegisterUlong(SdhcExtension, SDHC_ARGUMENT, Command->Argument);

    CommandReg = Command->Index << 8;
    switch (Command->ResponseType) {
    case SdResponseTypeNone:
        break;

    case SdResponseTypeR1:
    case SdResponseTypeR5:
    case SdResponseTypeR6:
        CommandReg |= SDHC_CMD_RESPONSE_48BIT_NOBUSY | 
                      SDHC_CMD_CRC_CHECK_ENABLE | 
                      SDHC_CMD_INDEX_CHECK_ENABLE;
        break;

    case SdResponseTypeR1B:
    case SdResponseTypeR5B:
        CommandReg |= SDHC_CMD_RESPONSE_48BIT_WBUSY |
                      SDHC_CMD_CRC_CHECK_ENABLE | 
                      SDHC_CMD_INDEX_CHECK_ENABLE;
        break;

    case SdResponseTypeR2:
        CommandReg |= SDHC_CMD_RESPONSE_136BIT | 
                      SDHC_CMD_CRC_CHECK_ENABLE;
        break;

    case SdResponseTypeR3:
    case SdResponseTypeR4:
        CommandReg |= SDHC_CMD_RESPONSE_48BIT_NOBUSY;
        break;
    
    default:

        NT_ASSERTMSG("SDHC - Invalid response type", FALSE);

        return STATUS_INVALID_PARAMETER;
    }

    if (Request->Command.TransferType != SdTransferTypeNone) {
        TransferMode = 0;
        CommandReg |= SDHC_CMD_DATA_PRESENT;

    } else {
        TransferMode = 
            SdhcReadRegisterUshort(SdhcExtension, SDHC_TRANSFER_MODE);

        TransferMode &= ~SDHC_TM_DMA_ENABLE;
        TransferMode &= ~SDHC_TM_AUTO_CMD12_ENABLE;
        TransferMode &= ~SDHC_TM_AUTO_CMD23_ENABLE;
        SdhcWriteRegisterUshort(SdhcExtension,
                                SDHC_TRANSFER_MODE,
                                TransferMode);
    }

    switch (Command->Type) {
    case SdCommandTypeSuspend:
        CommandType = SDHC_CMD_TYPE_SUSPEND;
        break;

    case SdCommandTypeResume:
        CommandType = SDHC_CMD_TYPE_RESUME;
        break;

    case SdCommandTypeAbort:
        CommandType = SDHC_CMD_TYPE_ABORT;
        break;

    default:
        CommandType = 0;
        break;
    }

    //
    // Set the bitmask for the required events that will fire after
    // writing to the command register. Depending on the response
    // type or whether the command involves data transfer, we will need
    // to wait on a number of different events.
    //
    
    Request->RequiredEvents = SDHC_IS_CMD_COMPLETE;
    if ((Command->ResponseType == SdResponseTypeR1B) ||
        (Command->ResponseType == SdResponseTypeR5B)) {

        Request->RequiredEvents |= SDHC_IS_TRANSFER_COMPLETE;
    }

    if (Request->Command.TransferMethod == SdTransferMethodSgDma
#if NV_T214_SDR104
        || SdhcExtension->T214DmaActive
#endif
#if NV_T210_UHS_DMA
        || SdhcExtension->T210DmaActive
#endif
       ) {
        Request->RequiredEvents |= SDHC_IS_TRANSFER_COMPLETE;

    } else if (Request->Command.TransferMethod == SdTransferMethodPio) {
        if (Request->Command.TransferDirection == SdTransferDirectionRead) {
            Request->RequiredEvents |= SDHC_IS_BUFFER_READ_READY;

        } else {
            Request->RequiredEvents |= SDHC_IS_BUFFER_WRITE_READY;
        }
    }

    //
    // Issue the actual command.
    //

#if NV_T210_LITE_WINARM_WRITE
    // An accepted application prefix applies to one issued command only.
    SdhcExtension->AppPrefixAccepted = FALSE;
    if (Command->Index == 17 || Command->Index == 18) {
        SdhcExtension->NextReadLba = Command->Argument;
    } else {
        SdhcExtension->NextReadLba = MAXULONG;
    }
    SdhcExtension->IssuedWriteCount = 0;
    if (Command->Index == 24 || Command->Index == 25) {
        SdhcExtension->IssuedWriteIndex = Command->Index;
        SdhcExtension->IssuedWriteLba = Command->Argument;
        SdhcExtension->IssuedWriteCount = Command->BlockCount;
    }
#endif
#if NV_T210_UHS
    /* Freeze after the first 32 commands: the UHS attempt happens during card
       identification, long before runtime I/O would overwrite a rolling ring. */
    if (SdhcExtension->T210Enabled && SdhcExtension->T210CmdCount < 32u) {
        SdhcExtension->T210CmdRing[SdhcExtension->T210CmdCount] =
            (UCHAR)(Command->Index & 0x3fu);
        SdhcExtension->T210CmdCount = SdhcExtension->T210CmdCount + 1u;
        /* Interleave the interesting commands into the same timeline as the
           signalling/speed callbacks: 10=CMD11 11=CMD0 12=CMD6 13=CMD13
           14=CMD19 15=CMD7. */
        switch (Command->Index) {
        case 11: T210TraceAdd(SdhcExtension, 10u); break;
        case 0:  T210TraceAdd(SdhcExtension, 11u); break;
        case 6:  T210TraceAdd(SdhcExtension, 12u); break;
        case 13: T210TraceAdd(SdhcExtension, 13u); break;
        case 19: T210TraceAdd(SdhcExtension, 14u); break;
        case 7:  T210TraceAdd(SdhcExtension, 15u); break;
        }
    }
#endif
    CommandReg |= CommandType;
    SdhcWriteRegisterUshort(SdhcExtension, SDHC_COMMAND, CommandReg);

    //
    // We must wait until the request completes.
    //

    return STATUS_PENDING;
}

NTSTATUS
SdhcGetResponse(
    _In_ PSDHC_EXTENSION SdhcExtension,
    _In_ PSDPORT_COMMAND Command,
    _Out_ PVOID ResponseBuffer
    )

/*++

Routine Description:

    This routine reads the response of the SD card which is sent over
    the command line, and stores it in the specified buffer

Arguments:

    SdhcExtension - Host controller specific driver context.

    Request - Supplies the descriptor for this SD command.

Return value:

    STATUS_SUCCESS - Response successfully returned.

    STATUS_INVALID_PARAMETER - Invalid response length.

--*/

{

    UCHAR Index;
    PUCHAR Response;
    UCHAR ResponseLength;

    ResponseLength = SdhcGetResponseLength(Command);
    if (ResponseLength > 16) {
        return STATUS_INVALID_PARAMETER;
    }

    Response = (PUCHAR) ResponseBuffer;
    for (Index = 0; Index < ResponseLength; Index += 1) {

#pragma prefast(suppress: 22103, "Response length is guaranteed to be <= ResponseLength.")

        Response[Index] = 
            SdhcReadRegisterUchar(SdhcExtension, SDHC_RESPONSE + Index);
    }

    return STATUS_SUCCESS;
}

NTSTATUS
SdhcSetTransferMode(
    _In_ PSDHC_EXTENSION SdhcExtension,
    _In_ PSDPORT_REQUEST Request
    )

/*++

Routine Description:

    This routine sets up the host for a data transfer.

Arguments:

    SdhcExtension - Host controller specific driver context.

    Command - Supplies the descriptor for this SD command.

Return value:

    STATUS_SUCCESS - Transfer mode successfully set.

    STATUS_INVALID_PARAMETER - Invalid command transfer parameters.

--*/

{

    USHORT BlockCount;
    USHORT BlockSize;
    USHORT TransferMode;

    NT_ASSERT(Request->Command.TransferMethod != SdTransferMethodUndefined);

    BlockCount = Request->Command.BlockCount;
    if (Request->Command.BlockSize > 2048) {

        NT_ASSERTMSG("SDHC - Invalid block size for command", FALSE);

        return STATUS_INVALID_PARAMETER;
    }

    if ((Request->Command.TransferDirection != SdTransferDirectionRead) &&
        (Request->Command.TransferDirection != SdTransferDirectionWrite)) {

        return STATUS_INVALID_PARAMETER;
    }

    TransferMode = 0;
    TransferMode &= ~(SDHC_TM_AUTO_CMD12_ENABLE |
                      SDHC_TM_AUTO_CMD23_ENABLE |
                      SDHC_TM_DMA_ENABLE |
                      SDHC_TM_BLKCNT_ENABLE |
                      SDHC_TM_MULTIBLOCK);

    // if (Command->TransferType = TransferTypeMultiBlockNoStop) {

    //}

    if (BlockCount > 1 || (NV_T210_LITE_WINARM_WRITE && Request->Command.Index == 25)) {
        TransferMode |= SDHC_TM_MULTIBLOCK;
        TransferMode |= SDHC_TM_BLKCNT_ENABLE;
        TransferMode |= SDHC_TM_AUTO_CMD12_ENABLE;
    }

    if (Request->Command.TransferMethod == SdTransferMethodSgDma) {
        TransferMode |= SDHC_TM_DMA_ENABLE;

    } else {

        NT_ASSERT(Request->Command.TransferMethod == SdTransferMethodPio);

    }

    BlockSize = Request->Command.BlockSize;
    TransferMode &= ~SDHC_TM_TRANSFER_READ;
    if (Request->Command.TransferDirection == SdTransferDirectionRead) {
        TransferMode |= SDHC_TM_TRANSFER_READ;
    }

    SdhcWriteRegisterUlong(SdhcExtension, SDHC_SYSADDR, BlockCount);
    SdhcWriteRegisterUshort(SdhcExtension, SDHC_BLOCK_SIZE, BlockSize);
    SdhcWriteRegisterUshort(SdhcExtension, SDHC_BLOCK_COUNT, BlockCount);
    SdhcWriteRegisterUshort(SdhcExtension, SDHC_TRANSFER_MODE, TransferMode);
    return STATUS_SUCCESS;
}

VOID
SdhcReadDataPort(
    _In_ PSDHC_EXTENSION SdhcExtension,
    _Out_writes_all_(Length) PUCHAR Buffer,
    _In_ SIZE_T Length
    )

/*++

Routine Description:

    The data port must be accessed maintaining DWORD alignment. So for example:

        IN DWORD 130
        IN DWORD 130

    is the same as

        IN USHORT 130
        IN USHORT 132
        IN UCHAR  130
        IN UCHAR  131
        IN UCHAR  132
        IN UCHAR  133

Arguments:

    SdhcExtension - Host controller specific driver context.

    Buffer - Data buffer to read.

    Length - Length of the data buffer in bytes.

Return value:

    None.

--*/

{

    PUCHAR Port;

    Port = (PUCHAR) SdhcExtension->BaseAddress + SDHC_DATA_PORT;
#if NV_T210_FAST_PIO
    /* The data port is a FIFO: the same address is read repeatedly. Do it
       inline instead of one SDPORT helper call per dword. */
    {
        volatile ULONG *fifo = (volatile ULONG *) Port;
        PULONG out = (PULONG) Buffer;
        SIZE_T words = Length / sizeof(ULONG);
        SIZE_T k;
        for (k = 0; k < words; ++k) out[k] = READ_REGISTER_ULONG(fifo);
        Buffer += words * sizeof(ULONG);
        Length -= words * sizeof(ULONG);
    }
#endif

#if NV_PIO_BUFFERED
    // One buffered read for the whole block instead of one call per DWORD.
    if (Length >= sizeof(ULONG)) {
        SdhcReadRegisterBufferUlong(SdhcExtension,
                                    SDHC_DATA_PORT,
                                    (PULONG) Buffer,
                                    Length / sizeof(ULONG));

        Buffer += Length & ~(sizeof(ULONG) - 1);
        Length &= sizeof(ULONG) - 1;
    }
#else
    while (Length >= sizeof(ULONG)) {
        SdhcReadRegisterBufferUlong(SdhcExtension,
                                    SDHC_DATA_PORT,
                                    (PULONG) Buffer,
                                    1);

        Buffer += sizeof(ULONG);
        Length -= sizeof(ULONG);
    }
#endif

    if (Length >= sizeof(USHORT)) {
        SdhcReadRegisterBufferUshort(SdhcExtension,
                                     SDHC_DATA_PORT,
                                     (PUSHORT) Buffer,
                                     1);

        Buffer += sizeof(USHORT);
        Length -= sizeof(USHORT);
    }

    if (Length >= sizeof(UCHAR)) {
        SdhcReadRegisterBufferUchar(SdhcExtension, 
                                    SDHC_DATA_PORT + sizeof(USHORT),
                                    Buffer,
                                    1);
    }
}

VOID
SdhcWriteDataPort(
    _In_ PSDHC_EXTENSION SdhcExtension,
    _In_reads_(Length) PUCHAR Buffer,
    _In_ ULONG Length
    )

/*++

Routine Description:

    The data port must be accessed maintaining DWORD alignment. So for example:

        IN DWORD 130
        IN DWORD 130

    is the same as

        IN USHORT 130
        IN USHORT 132
        IN UCHAR  130
        IN UCHAR  131
        IN UCHAR  132
        IN UCHAR  133

Arguments:

    SdhcExtension - Host controller specific driver context.

    Buffer - Data buffer to write.

    Length - Length of the data buffer in bytes.

Return value:

    None.

--*/

{

#if NV_T210_FAST_PIO
    /* Same as the read side: the data port is a FIFO, so write the same address
       repeatedly instead of paying one SDPORT helper call per dword. This used
       to sit inside NV_PIO_BUFFERED, which is why the fast path never ran for
       writes and the write rate stayed at the 2.4 MB/s of the slow loop. */
    {
        volatile ULONG *fifo = (volatile ULONG *)((PUCHAR) SdhcExtension->BaseAddress + SDHC_DATA_PORT);
        PULONG in = (PULONG) Buffer;
        ULONG words = Length / sizeof(ULONG);
        ULONG k;
        for (k = 0; k < words; ++k) WRITE_REGISTER_ULONG(fifo, in[k]);
        Buffer += words * sizeof(ULONG);
        Length -= words * sizeof(ULONG);
    }
#endif

#if NV_PIO_BUFFERED
    // One buffered write for the whole block instead of one call per DWORD.
    if (Length >= sizeof(ULONG)) {
        SdhcWriteRegisterBufferUlong(SdhcExtension,
                                     SDHC_DATA_PORT,
                                     (PULONG) Buffer,
                                     Length / sizeof(ULONG));

        Buffer += Length & ~(sizeof(ULONG) - 1);
        Length &= sizeof(ULONG) - 1;
    }
#else
    while (Length >= sizeof(ULONG)) {
        SdhcWriteRegisterBufferUlong(SdhcExtension,
                                     SDHC_DATA_PORT,
                                     (PULONG) Buffer,
                                     1);

        Buffer += sizeof(ULONG);
        Length -= sizeof(ULONG);
    }
#endif

    if (Length >= sizeof(USHORT)) {
        SdhcWriteRegisterBufferUshort(SdhcExtension,
                                      SDHC_DATA_PORT,
                                      (PUSHORT) Buffer,
                                      1);

        Buffer += sizeof(USHORT);
        Length -= sizeof(USHORT);
    }

    if (Length >= sizeof(UCHAR)) {
        SdhcWriteRegisterBufferUchar(SdhcExtension,
                                     SDHC_DATA_PORT + sizeof(USHORT),
                                     Buffer,
                                     1);
    }

    NT_ASSERT((SdhcReadRegisterUshort(SdhcExtension, SDHC_ERROR_STATUS) & 
              SDHC_ES_BAD_DATA_SPACE_ACCESS) == 0);
}

NTSTATUS
SdhcBuildTransfer(
    _In_ PSDHC_EXTENSION SdhcExtension,
    _In_ PSDPORT_REQUEST Request
    )

/*++

Routine Description:

    Prepare the transfer request.

Arguments:

    SdhcExtension - Host controller specific driver context.

    Command - The command for which we're building the transfer request.

Return value:

    NTSTATUS

--*/

{

    NTSTATUS Status;

    NT_ASSERT(Request->Command.TransferType != SdTransferTypeNone);

    NT_ASSERT(Request->Command.TransferMethod != SdTransferMethodUndefined);

    switch (Request->Command.TransferMethod) {
    case SdTransferMethodPio:
        Status = SdhcBuildPioTransfer(SdhcExtension, Request);
        break;

    case SdTransferMethodSgDma:
        Status = SdhcBuildAdmaTransfer(SdhcExtension, Request);
        break;

    default:
        Status = STATUS_NOT_SUPPORTED;
        break;
    }

    return Status;
}

NTSTATUS
SdhcStartTransfer(
    _In_ PSDHC_EXTENSION SdhcExtension,
    _In_ PSDPORT_REQUEST Request
    )

/*++

Routine Description:

    Execute the transfer request.

Arguments:

    SdhcExtension - Host controller specific driver context.

    Request - The command for which we're building the transfer request.

Return value:

    NTSTATUS

--*/

{

    NTSTATUS Status;

    NT_ASSERT(Request->Command.TransferType != SdTransferTypeNone);
#if NV_T214_SDR104
    if (SdhcExtension->T214DmaComplete) {
        SdhcExtension->T214DmaComplete = FALSE;
        Request->Command.BlockCount = 0;
        Request->Status = STATUS_SUCCESS;
        SdhcExtension->OutstandingRequests[0] = NULL;
        SdPortCompleteRequest(Request, STATUS_SUCCESS);
        return STATUS_SUCCESS;
    }
#endif

    switch (Request->Command.TransferMethod) {
    case SdTransferMethodPio:
        Status = SdhcStartPioTransfer(SdhcExtension, Request);
        break;

    case SdTransferMethodSgDma:
        Status = SdhcStartAdmaTransfer(SdhcExtension, Request);
        break;

    default:
        Status = STATUS_NOT_SUPPORTED;
        break;
    }

    return Status;
}

NTSTATUS
SdhcBuildPioTransfer(
    _In_ PSDHC_EXTENSION SdhcExtension,
    _In_ PSDPORT_REQUEST Request
    )

/*++

Routine Description:

    Prepare the PIO transfer request.

Arguments:

    SdhcExtension - Host controller specific driver context.

    Request - The command for which we're building the transfer.

Return value:

    NTSTATUS

--*/

{

    NTSTATUS status = SdhcSetTransferMode(SdhcExtension, Request);
#if NV_T214_SDR104
    if (SdhcExtension->T214DmaFault) return STATUS_IO_DEVICE_ERROR;
    if (NT_SUCCESS(status)) (VOID)T214BuildDma(SdhcExtension, Request);
#endif
#if NV_T210_UHS_DMA
    /* T210: same trick as T214 - SDPORT keeps its PIO contract, the hardware
       moves the blocks with ADMA32 out of the non-cached bounce buffer.
       T210BuildDma returns FALSE (and PIO is used) once DMA is faulted, so a
       DMA error degrades the slot instead of failing every request. */
    if (NT_SUCCESS(status) && SdhcExtension->T210Enabled) {
        (VOID)T210BuildDma(SdhcExtension, Request);
    }
#endif
    return status;
}


NTSTATUS
SdhcBuildAdmaTransfer(
    _In_ PSDHC_EXTENSION SdhcExtension,
    _In_ PSDPORT_REQUEST Request
    )

/*++

Routine Description:

    Prepare the ADMA2 transfer request.

Arguments:

    SdhcExtension - Host controller specific driver context.

    Request - The command for which we're building the transfer.

Return value:

    NTSTATUS

--*/

{

    NTSTATUS Status;
    ULONG TransferLength;

    Status = SdhcSetTransferMode(SdhcExtension, Request);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    NT_ASSERT(Request->Command.ScatterGatherList != NULL);

    //
    // Create the ADMA2 descriptor table in the host's DMA buffer.
    //

    Status = 
        SdhcCreateAdmaDescriptorTable(
            Request, 
            (BOOLEAN) SdhcExtension->Capabilities.Supported.Address64Bit,
            &TransferLength);

    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    SdhcWriteRegisterUlong(SdhcExtension, 
                           SDHC_ADMA_SYSADDR_LOW, 
                           Request->Command.DmaPhysicalAddress.LowPart);

    if (SdhcExtension->Capabilities.Supported.Address64Bit) {
        SdhcWriteRegisterUlong(SdhcExtension,
                               SDHC_ADMA_SYSADDR_HIGH,
                               Request->Command.DmaPhysicalAddress.HighPart);
    } else {

        NT_ASSERT(Request->Command.DmaPhysicalAddress.HighPart == 0);

    }

    if (Request->Command.TransferDirection == SdTransferDirectionRead) {
        NvDiag.BlocksRead += Request->Command.BlockCount;

    } else {
        NvDiag.BlocksWritten += Request->Command.BlockCount;
    }

    NvDiag.SgDmaRequests += 1;
    NvDiag.SgDmaBlocks += Request->Command.BlockCount;

    return STATUS_SUCCESS;
}

NTSTATUS
SdhcStartPioTransfer(
    _In_ PSDHC_EXTENSION SdhcExtension,
    _In_ PSDPORT_REQUEST Request
    )

/*++

Routine Description:

    Execute the PIO transfer request.

Arguments:

    SdhcExtension - Host controller specific driver context.

    Request - The command for which we're building the transfer.

Return value:

    NTSTATUS

--*/

{

    NT_ASSERT((Request->Command.TransferDirection == SdTransferDirectionRead) ||
              (Request->Command.TransferDirection == SdTransferDirectionWrite));

#if NV_T210_UHS_DMA
    /* The hardware is moving this transfer (ADMA32 through the bounce buffer);
       touching the data port here would race the DMA engine. */
    if (SdhcExtension->T210DmaActive) {
        return STATUS_SUCCESS;
    }
#endif

#if NV_PIO_POLL
    BOOLEAN Read;
    ULONG Moved;
    ULONG ReadyBit;
    ULONG Waited;
    LONGLONG CallStart;

    CallStart = NvPioNow();
    Read = (Request->Command.TransferDirection == SdTransferDirectionRead);
    ReadyBit = Read ? SDHC_PS_BUFFER_READ_ENABLE : SDHC_PS_BUFFER_WRITE_ENABLE;
    for (Moved = 0; Moved < NV_PIO_POLL_BLOCKS; Moved += 1) {
        LONGLONG PollStart;
        LONGLONG PollEnd;
        LONGLONG PortStart;

        //
        // The buffer-ready event that started this call may be for a block
        // already moved by polling, so move data only when the buffer is ready.
        // Stop on an error: the interrupt reports it.
        //

        PollStart = NvPioNow();
        for (Waited = 0; ; Waited += 1) {
            if ((SdhcReadRegisterUlong(SdhcExtension, SDHC_PRESENT_STATE) & ReadyBit) != 0) {
                break;
            }

            if ((Waited >= NV_PIO_POLL_US) ||
                ((SdhcReadRegisterUshort(SdhcExtension, SDHC_INTERRUPT_STATUS) &
                  SDHC_IS_ERROR_INTERRUPT) != 0)) {

                NvDiag.PioBudgetHit += 1;
                break;
            }

            NvDiag.PioPollIters += 1;
            KeStallExecutionProcessor(1);
        }

        PollEnd = NvPioNow();
        NvDiag.PioPollTicks += PollEnd - PollStart;

        if ((SdhcReadRegisterUlong(SdhcExtension, SDHC_PRESENT_STATE) & ReadyBit) == 0) {
            NvDiag.PioReadyMiss += 1;
            break;
        }

        PortStart = NvPioNow();
        if (Read) {
            SdhcReadDataPort(SdhcExtension,
                             Request->Command.DataBuffer,
                             Request->Command.BlockSize);
#if NV_T210_UHS
            /* Snoop the two card answers that decide UHS-I support, without
               changing the flow: ACMD51 (SCR, SD_SPEC3 = UHS) and the CMD6
               function table (group 1 = supported speed modes). */
            if (SdhcExtension->T210Enabled) {
                if (Request->Command.Index == 51 &&
                    Request->Command.Class == SdCommandClassApp) {
                    RtlCopyMemory(&SdhcExtension->T210Scr, Request->Command.DataBuffer,
                                  min(Request->Command.BlockSize, 16));
                } else if (Request->Command.Index == 6 &&
                           Request->Command.Class == SdCommandClassStandard) {
                    RtlCopyMemory(&SdhcExtension->T210FnTable, Request->Command.DataBuffer,
                                  min(Request->Command.BlockSize, 64));
                }
            }
#endif
#if NV_T210_LITE_WINARM_WRITE
            LiteObserveRead(SdhcExtension, &Request->Command);
#endif

            NvDiag.BlocksRead += 1;

        } else {
            SdhcWriteDataPort(SdhcExtension,
                              Request->Command.DataBuffer,
                              Request->Command.BlockSize);

            NvDiag.BlocksWritten += 1;
        }
        NvDiag.PioPortTicks += NvPioNow() - PortStart;

        Request->Command.BlockCount -= 1;
        if (Request->Command.BlockCount == 0) {
            Request->RequiredEvents |= SDHC_IS_TRANSFER_COMPLETE;
            Request->Status = STATUS_SUCCESS;
            NvDiag.PioCalls += 1;
            NvDiag.PioCallBlocks += Moved + 1;
            NvDiag.PioCallTicks += NvPioNow() - CallStart;
            return STATUS_PENDING;
        }

        Request->Command.DataBuffer += Request->Command.BlockSize;
    }

    NvDiag.PioCalls += 1;
    NvDiag.PioCallBlocks += Moved;
    NvDiag.PioCallMore += 1;
    NvDiag.PioCallTicks += NvPioNow() - CallStart;

    //
    // More blocks to come: wait for the next buffer-ready interrupt.
    //

    Request->RequiredEvents |= Read ? SDHC_IS_BUFFER_READ_READY : SDHC_IS_BUFFER_WRITE_READY;
    Request->Status = STATUS_MORE_PROCESSING_REQUIRED;
    return STATUS_PENDING;
#else
    if (Request->Command.TransferDirection == SdTransferDirectionRead) {
        SdhcReadDataPort(SdhcExtension,
                         Request->Command.DataBuffer,
                         Request->Command.BlockSize);
#if NV_T210_LITE_WINARM_WRITE
        LiteObserveRead(SdhcExtension, &Request->Command);
#endif

        NvDiag.BlocksRead += 1;

    } else {
        SdhcWriteDataPort(SdhcExtension,
                          Request->Command.DataBuffer,
                          Request->Command.BlockSize);

        NvDiag.BlocksWritten += 1;
    }

    Request->Command.BlockCount -= 1;
    if (Request->Command.BlockCount >= 1) {
        Request->Command.DataBuffer += Request->Command.BlockSize;
        if (Request->Command.TransferDirection == SdTransferDirectionRead) {
            Request->RequiredEvents |= SDHC_IS_BUFFER_READ_READY;

        } else {
            Request->RequiredEvents |= SDHC_IS_BUFFER_WRITE_READY;
        }

        Request->Status = STATUS_MORE_PROCESSING_REQUIRED;

    } else {

        NT_ASSERT(Request->Command.BlockCount == 0);

        Request->RequiredEvents |= SDHC_IS_TRANSFER_COMPLETE;
        Request->Status = STATUS_SUCCESS;
    }

    return STATUS_PENDING;
#endif
}

NTSTATUS
SdhcStartAdmaTransfer(
    _In_ PSDHC_EXTENSION SdhcExtension,
    _In_ PSDPORT_REQUEST Request
    )

/*++

Routine Description:

    Execute the ADMA2 transfer request.

Arguments:

    SdhcExtension - Host controller specific driver context.

    Request - The command for which we're building the transfer.

Return value:

    NTSTATUS

--*/

{

    UNREFERENCED_PARAMETER(SdhcExtension);

    Request->Status = STATUS_SUCCESS;
    SdPortCompleteRequest(Request, Request->Status);
    return STATUS_SUCCESS;
}

USHORT
SdhcCalcClockFrequency(
    _In_ PSDHC_EXTENSION SdhcExtension,
    _In_ ULONG TargetFrequency,
    _Out_opt_ PULONG ActualFrequency
    )

/*++

Routine Description:

    Execute the ADMA2 transfer request.

Arguments:

    SdhcExtension - Host controller specific driver context.

    TargetFrequency - The frequency in kHz to which we want to set the bus.

    ActualFrequency - The actual frequency to which the bus is set.

Return value:

    ClockControl - The value of the Clock Control register to be written.

--*/

{

    ULONG BaseFrequency;
    USHORT ClockControl;
    ULONG Divisor;
    USHORT SpecVersion;

    *ActualFrequency = 0;
    BaseFrequency = SdhcExtension->Capabilities.BaseClockFrequencyKhz;
    Divisor = MAX(BaseFrequency / TargetFrequency, 1);

    NT_ASSERT(Divisor > 0);

    SpecVersion = SdhcReadRegisterUshort(SdhcExtension, SDHC_VERSION) & 0xFF;
    if (SpecVersion < SDHC_SPEC_VERSION_3) {

        //
        // Calculate the fastest available clock frequency which is <= 
        // tthe requested frequency.
        //

        Divisor = 1;
        while (((BaseFrequency / Divisor) > TargetFrequency) &&
               (Divisor < SDHC_MAX_CLOCK_DIVISOR)) {

            Divisor <<= 1;
        }

        *ActualFrequency = BaseFrequency / Divisor;
        Divisor >>= 1;
        ClockControl = ((USHORT) Divisor << 8);

    } else {

        //
        // Host controller version 3.0 supports the 10-bit divided clock mode.
        // The hardware rule is SDCLK = BaseClock / (2 * Divisor), so Divisor 0
        // means "base / 2", never "undivided". Pick the Divisor whose rate is
        // closest to the request (round to nearest) and never emit 0.
        //

        Divisor = (BaseFrequency + TargetFrequency) / (2 * TargetFrequency);

        if (Divisor == 0) {
            Divisor = 1;
        }

        if (Divisor > SDHC_MAX_CLOCK_DIVISOR_SPEC_3 / 2) {
            Divisor = SDHC_MAX_CLOCK_DIVISOR_SPEC_3 / 2;
        }

        *ActualFrequency = BaseFrequency / Divisor / 2;

        ClockControl = ((USHORT) (Divisor & 0xFF)) << 8;
        ClockControl |= ((USHORT) ((Divisor >> 8) & 0x03)) << 6;
    }

    return ClockControl;
}

NTSTATUS
SdhcCreateAdmaDescriptorTable(
    _In_ PSDPORT_REQUEST Request,
    _In_ BOOLEAN Use64BitDescriptor,
    _Out_ PULONG TotalTransferLength
    )

/*++

Routine Description:

    This routine creates a ADMA descriptor table based on scattor gatther list
    given by Sglist.

Arguments:

    Socket - Supplies the pointer to the socket

    Request - Data transfer request for which to build the descriptor table.

    TotalTransferLength - Supplies the pointer to return the total transfer 
                          length of the descriptor table

Return value:

    Whether the table was successfully created.

--*/

{

    PUCHAR Buffer;
    PSDHC_ADMA2_DESCRIPTOR_TABLE_ENTRY Descriptor;
    ULONG NumberOfElements;
    PHYSICAL_ADDRESS NextAddress;
    ULONG NextLength;
    ULONG RemainingLength;
    PSCATTER_GATHER_ELEMENT SglistElement;
    
    Buffer = Request->Command.DmaVirtualAddress;
    Descriptor = NULL;
    NumberOfElements = Request->Command.ScatterGatherList->NumberOfElements;
    SglistElement = &Request->Command.ScatterGatherList->Elements[0];
    *TotalTransferLength = 0;

    NT_ASSERT(NumberOfElements > 0);

    //
    // Iterate through each element in the SG list and convert it into the
    // descriptor table required by the controller.
    //

    while (NumberOfElements > 0) {
        RemainingLength = SglistElement->Length;
        NextAddress.QuadPart = SglistElement->Address.QuadPart;

        NT_ASSERT(RemainingLength > 0);

        while (RemainingLength > 0) {
            Descriptor = (PSDHC_ADMA2_DESCRIPTOR_TABLE_ENTRY) Buffer;
            Buffer += sizeof(SDHC_ADMA2_DESCRIPTOR_TABLE_ENTRY);

            NT_ASSERT((ULONG_PTR) Buffer <
                      (ULONG_PTR) Request->Command.DmaVirtualAddress + 
                                  Request->Command.Length);

            NextLength = MIN(SDHC_ADMA2_MAX_LENGTH_PER_ENTRY, RemainingLength);
            RemainingLength -= NextLength;

            //
            // Set the entry attributes and length.
            //

            Descriptor->AsUlong = 0;
            Descriptor->Action = SDHC_ADMA2_ACTION_TRAN;
            Descriptor->Attribute = SDHC_ADMA2_ATTRIBUTE_VALID;
            Descriptor->Length = NextLength;
            *TotalTransferLength += NextLength;

            //
            // Set the address field.
            //

            if (Use64BitDescriptor) {
                *((PULONGLONG) Buffer) = NextAddress.QuadPart;
                Buffer += sizeof(LONGLONG);

            } else {

                //
                // The HighPart should not be a non-zero value, since in the
                // DMA adapter object, we declared that this device only
                // supports 32-bit addressing.
                //

                NT_ASSERT(NextAddress.HighPart == 0);

                *((PULONG) Buffer) = NextAddress.LowPart;
                Buffer += sizeof(ULONG);
            }

            NextAddress.QuadPart += NextLength;
        }

        SglistElement += 1;
        NumberOfElements -= 1;

        //
        // Set the END bit if we're at the last element.
        //

        if (NumberOfElements == 0) {
            Descriptor->Attribute |= SDHC_ADMA2_ATTRIBUTE_END;
        }
    }

    return STATUS_SUCCESS;
}

VOID
SdhcInitializePciConfigSpace(
    _In_ PSD_MINIPORT Miniport
    )

/*++

Routine Description:

    This routine updates the PCI configuration space.

Arguments:

    Argument - Pointer to the functional device object extension for the SD host controller

Return value:

    None.

--*/

{








    if (Miniport->ConfigurationInfo.BusType != SdBusTypePci) {
        return;
    }

    //
    // Some PCI based controllers require access to PCI configuration
    // space to enable certain features. In order to do this, sdport
    // provides two APIs, SdPortGetPciConfigSpace and 
    // SdPortSetPciConfigSpace to access various PCI configuration 
    // registers. For example, to access offset 0xA0 and unset bit
    // 31 (hypothetically to turn on or off a feature):
    //  
    // ULONG PciConfig;
    //
    // PciConfig = 0;
    // SdPortGetPciConfigSpace(Miniport,
    //                         0xA0,
    //                         (PUCHAR)&PciConfig,
    //                         sizeof(PciConfig));
    //
    // PciConfig &= ~(1 << 31);
    // SdPortSetPciConfigSpace(Miniport,
    //                         0xA0,
    //                         (PUCHAR)&PciConfig,
    //                         sizeof(PciConfig));
    //
    























































































































}










































