/* SPDX-License-Identifier: GPL-2.0-only */
/* Mariko Switch Lite GPU PMIC SPB serialization and Power Mode bridge.
 * Reuses SwitchFan's single MAX77812 SPB connection under PmicLock so CPU M4
 * and GPU M1 never issue concurrent I2C5 transfers.
 */
#ifndef SWITCHFAN_T214_GPU_PMIC_H
#define SWITCHFAN_T214_GPU_PMIC_H

#include "../power_ioctl.h"

#define TG_GPU_IOCTL_PMIC             0x00222410u
#define TG_GPU_PMIC_QUERY_ADDRESS     0x100u
#define TG_GPU_PMIC_QUERY_PLLP        0x101u
#define TG_GPU_PMIC_QUERY_TEMP        0x102u
#define TG_GPU_PMIC_QUERY_EMC_KHZ     0x103u
#define TG_GPU_PMIC_QUERY_DVFS_POLICY 0x104u
#define TG_GPU_PMIC_REPORT_GPU_KHZ    0x105u
#define TG_GPU_PMIC_REPORT_GPU_UV     0x106u
#define TG_GPU_PMIC_REPORT_GPU_STATUS 0x107u
#define TG_GPU_PMIC_REPORT_ABI        0x108u
#define TG_GPU_LIVE_COUNTER_ABI       0x00020001u

#define TG_GPU_DVFS_POLICY_PACK(mode, min_khz, max_khz, seq) \
    ((((ULONG)(mode)) & 0xfu) | \
     (((((ULONG)(min_khz)) / 400u) & 0xfffu) << 4) | \
     (((((ULONG)(max_khz)) / 400u) & 0xfffu) << 16) | \
     ((((ULONG)(seq)) & 0xfu) << 28))
#define TG_GPU_DVFS_POLICY_MODE(v)    (((ULONG)(v)) & 0xfu)
#define TG_GPU_DVFS_POLICY_MIN_KHZ(v) (((((ULONG)(v)) >> 4) & 0xfffu) * 400u)
#define TG_GPU_DVFS_POLICY_MAX_KHZ(v) (((((ULONG)(v)) >> 16) & 0xfffu) * 400u)

#define GPU_M1_ENABLE_REG 0x06u
#define GPU_M1_VOUT_REG   0x23u
#define GPU_M1_MIN_CODE   68u  /* 250 + 68*5  = 590 mV (T210B01 HighOPT Vmin) */
#define GPU_M1_MAX_CODE   160u /* 250 + 160*5 = 1050 mV (T210B01 rail ceiling) */

struct tg_gpu_pmic_request {
    ULONG address;
    ULONG reg;
    ULONG write;
    ULONG value;
};

static BOOLEAN SwFanGpuLiveCounter(PSWFAN_DEVICE Dev)
{
    return Dev->GpuAbiTime && KeQueryInterruptTime() - Dev->GpuAbiTime <= 20000000ULL;
}

static NTSTATUS SwFanGpuPmic(PSWFAN_DEVICE Dev, WDFREQUEST Request, ULONG_PTR *Info)
{
    struct tg_gpu_pmic_request *In, *Out, Op;
    WDF_REQUEST_SEND_OPTIONS Options;
    WDF_MEMORY_DESCRIPTOR Memory;
    UCHAR Buffer[2], Readback = 0, EnCtrl = 0, M1Code = 0;
    ULONG_PTR Bytes;
    NTSTATUS Status;

    if (WdfRequestGetRequestorMode(Request) != KernelMode)
        return STATUS_ACCESS_DENIED;
    Status = WdfRequestRetrieveInputBuffer(Request, sizeof(Op), (PVOID *)&In, NULL);
    if (!NT_SUCCESS(Status)) return Status;
    Op = *In;
    Status = WdfRequestRetrieveOutputBuffer(Request, sizeof(Op), (PVOID *)&Out, NULL);
    if (!NT_SUCCESS(Status)) return Status;
    if (!Dev->IsLite || Dev->Stopping || !Dev->Pmic || !Dev->CpuConfigured)
        return STATUS_DEVICE_NOT_READY;

    if (!Op.write && Op.reg >= TG_GPU_PMIC_QUERY_ADDRESS) {
        if (Op.reg == TG_GPU_PMIC_QUERY_ADDRESS) {
            Op.value = Dev->Config.PmicAddress;
        } else if (Op.reg == TG_GPU_PMIC_QUERY_PLLP) {
            if (Dev->Car == NULL) return STATUS_DEVICE_NOT_READY;
            Op.value = SwCpuRead(Dev, 0xa0);
        } else if (Op.reg == TG_GPU_PMIC_QUERY_TEMP) {
            if (!Dev->TempValid) return STATUS_DEVICE_NOT_READY;
            Op.value = Dev->LastTemp;
        } else if (Op.reg == TG_GPU_PMIC_QUERY_EMC_KHZ) {
            if (!Dev->Config.RamActualKhz) return STATUS_DEVICE_NOT_READY;
            Op.value = Dev->Config.RamActualKhz;
        } else if (Op.reg == TG_GPU_PMIC_QUERY_DVFS_POLICY) {
            Dev->GpuBridgeSeen = TRUE;
            Op.value = (ULONG)InterlockedCompareExchange(&Dev->GpuDvfsPolicy, 0, 0);
        } else {
            return STATUS_INVALID_PARAMETER;
        }
        *Out = Op;
        *Info = sizeof(Op);
        return STATUS_SUCCESS;
    }

    if (Op.write == 1 && Op.reg >= TG_GPU_PMIC_REPORT_GPU_KHZ) {
        if (Op.reg == TG_GPU_PMIC_REPORT_GPU_KHZ) {
            Dev->GpuBridgeSeen = TRUE;
            Dev->GpuReportedKhz = Op.value;
            Dev->GpuSampleTime = Op.value && SwFanGpuLiveCounter(Dev) ? KeQueryInterruptTime() : 0;
        } else if (Op.reg == TG_GPU_PMIC_REPORT_GPU_UV) {
            Dev->GpuBridgeSeen = TRUE;
            Dev->GpuReportedUv = Op.value;
        } else if (Op.reg == TG_GPU_PMIC_REPORT_GPU_STATUS) {
            Dev->GpuBridgeSeen = TRUE;
            Dev->GpuStatusWord = Op.value;
        } else if (Op.reg == TG_GPU_PMIC_REPORT_ABI) {
            Dev->GpuAbiTime = Op.value == TG_GPU_LIVE_COUNTER_ABI ? KeQueryInterruptTime() : 0;
        } else {
            return STATUS_INVALID_PARAMETER;
        }
        *Out = Op;
        *Info = sizeof(Op);
        return STATUS_SUCCESS;
    }

    if (Op.address != Dev->Config.PmicAddress || Op.write > 1 || Op.value > 255 ||
        (Op.reg != GPU_M1_ENABLE_REG && Op.reg != GPU_M1_VOUT_REG &&
         (Op.reg != CPU_M4_VOUT || Op.write)))
        return STATUS_INVALID_PARAMETER;

    SwFanPmicLock(Dev);
    if (Op.write) {
        Buffer[0] = (UCHAR)Op.reg;
        Buffer[1] = (UCHAR)Op.value;
        if (Op.reg == GPU_M1_VOUT_REG) {
            if (Op.value > GPU_M1_MAX_CODE) {
                SwFanPmicUnlock(Dev);
                return STATUS_INVALID_PARAMETER;
            }
            if (Op.value < GPU_M1_MIN_CODE) {
                Status = SwFanReadReg(Dev->Pmic, GPU_M1_ENABLE_REG, &EnCtrl);
                if (!NT_SUCCESS(Status)) { SwFanPmicUnlock(Dev); return Status; }
                /* Never allow undervolting M1 below 590 mV while enabled;
                 * restoring original_voltage after M1 disable remains valid. */
                if ((EnCtrl & 1u) != 0) {
                    SwFanPmicUnlock(Dev);
                    return STATUS_INVALID_PARAMETER;
                }
            }
        } else if (Op.reg == GPU_M1_ENABLE_REG) {
            if ((Op.value & 1u) != 0) {
                if (!Dev->TempValid || Dev->LastTemp >= 80) {
                    SwFanPmicUnlock(Dev);
                    return STATUS_DEVICE_NOT_READY;
                }
                Status = SwFanReadReg(Dev->Pmic, GPU_M1_VOUT_REG, &M1Code);
                if (!NT_SUCCESS(Status)) { SwFanPmicUnlock(Dev); return Status; }
                if (M1Code < GPU_M1_MIN_CODE || M1Code > GPU_M1_MAX_CODE) {
                    SwFanPmicUnlock(Dev);
                    return STATUS_INVALID_DEVICE_STATE;
                }
            }
            Status = SwFanReadReg(Dev->Pmic, GPU_M1_ENABLE_REG, &Readback);
            if (!NT_SUCCESS(Status)) { SwFanPmicUnlock(Dev); return Status; }
            Buffer[1] = (UCHAR)((Readback & ~1u) | (Op.value & 1u));
        }
        WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Memory, Buffer, sizeof(Buffer));
        WDF_REQUEST_SEND_OPTIONS_INIT(&Options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
        Options.Timeout = WDF_REL_TIMEOUT_IN_MS(200);
        Bytes = 0;
        Status = WdfIoTargetSendWriteSynchronously(Dev->Pmic, NULL, &Memory, NULL, &Options, &Bytes);
        if (!NT_SUCCESS(Status)) { SwFanPmicUnlock(Dev); return Status; }
        if (Bytes != sizeof(Buffer)) { SwFanPmicUnlock(Dev); return STATUS_DEVICE_PROTOCOL_ERROR; }
    }
    Status = SwFanReadReg(Dev->Pmic, (UCHAR)Op.reg, &Readback);
    SwFanPmicUnlock(Dev);
    if (!NT_SUCCESS(Status)) return Status;
    if (Op.write && Readback != Buffer[1]) return STATUS_DEVICE_DATA_ERROR;
    if (Op.write && Op.reg == GPU_M1_VOUT_REG)
        Dev->GpuReportedUv = (250u + (ULONG)Readback * 5u) * 1000u;
    Op.value = Readback;
    *Out = Op;
    *Info = sizeof(Op);
    return STATUS_SUCCESS;
}

static VOID SwFanPublishGpuPolicy(PSWFAN_DEVICE Dev, ULONG Mode, ULONG Minimum, ULONG Maximum)
{
    ULONG Seq = (Dev->PolicySeq + 1u) & 0xfu;
    if (!Seq) Seq = 1u;
    Dev->PolicySeq = Seq;
    (VOID)InterlockedExchange(&Dev->GpuDvfsPolicy,
        (LONG)TG_GPU_DVFS_POLICY_PACK(Mode, Minimum, Maximum, Seq));
}

/* Do not retry an unsuccessful DFLL transition from the periodic timer.
 * Only an explicit baseline selection clears this failure latch. */
static NTSTATUS SwFanDvfsFailureLocked(PSWFAN_DEVICE Dev, NTSTATUS Cause)
{
    NTSTATUS Recovery = SwFanDfllStopLocked(Dev);
    Dev->Dfll.FaultLatched = TRUE;
    Dev->Dfll.LastStatus = NT_SUCCESS(Recovery) ? Cause : Recovery;
    Dev->CpuThrottled = TRUE;
    Dev->PowerMode = SwFanPowerModeDefaultFixed;
    SwFanPublishGpuPolicy(Dev, 0, 460800u, 460800u);
    SwFanTrace(L"CpuLastError", (ULONG)Dev->Dfll.LastStatus);
    return Dev->Dfll.LastStatus;
}

static NTSTATUS SwFanDvfsUpdateLocked(PSWFAN_DEVICE Dev, ULONG Temp, BOOLEAN Valid)
{
    NTSTATUS Status;
    Dev->LastTemp = Temp;
    Dev->TempValid = Valid;
    if (Dev->Dfll.FaultLatched) return Dev->Dfll.LastStatus;
    if (!Valid || Temp >= 96u)
        return SwFanDvfsFailureLocked(Dev, STATUS_DEVICE_NOT_READY);
    Status = SwFanDfllApplyProfileLocked(Dev, Dev->PowerMode);
    return NT_SUCCESS(Status) ? Status : SwFanDvfsFailureLocked(Dev, Status);
}

static NTSTATUS SwFanSetPowerMode(PSWFAN_DEVICE Dev, ULONG Mode)
{
    BOOLEAN ScreenOff = FALSE;
    ULONG GpuMode = 0, GpuMinKhz = 460800u, GpuMaxKhz = 460800u;
    NTSTATUS Status = STATUS_SUCCESS;
    if (Mode >= SwFanPowerModeCount) return STATUS_INVALID_PARAMETER;
    if (!Dev->IsLite) return STATUS_NOT_SUPPORTED;
    if (Mode != SwFanPowerModeDefaultFixed && !SwFanGpuLiveCounter(Dev)) return STATUS_NOT_SUPPORTED;
    SwFanCpuLock(Dev);
    if (Dev->Stopping || !Dev->CpuConfigured || !Dev->Pmic || !Dev->Car) {
        SwFanCpuUnlock(Dev); return STATUS_DEVICE_NOT_READY;
    }
    if (Dev->BacklightLock) WdfWaitLockAcquire(Dev->BacklightLock, NULL);
    ScreenOff = Dev->BacklightReady && Dev->DisplayState == 0;
    if (Dev->BacklightLock) WdfWaitLockRelease(Dev->BacklightLock);
    if (Mode == SwFanPowerModeDefaultFixed) {
        Status = SwFanDfllStopLocked(Dev);
        if (!NT_SUCCESS(Status)) { SwFanCpuUnlock(Dev); return Status; }
        Dev->Dfll.FaultLatched = FALSE;
        Dev->CpuThrottled = !Dev->TempValid || Dev->LastTemp >= 80u;
        Status = SwCpuApplyLocked(Dev, Dev->BaseCpuOverclockEnabled &&
                                  !ScreenOff && Dev->TempValid && !Dev->CpuThrottled);
    } else {
        const struct sw_nvpmodel_profile *profile = &sw_nvpmodel_profiles[Mode];
        if (ScreenOff) Status = STATUS_DEVICE_NOT_READY;
        else Status = SwFanDfllApplyProfileLocked(Dev, Mode);
        GpuMode = 1;
        GpuMinKhz = profile->gpu_min_khz ? profile->gpu_min_khz : 76800u;
        GpuMaxKhz = profile->gpu_max_khz > 1228800u ? 1228800u : profile->gpu_max_khz;
    }
    if (!NT_SUCCESS(Status)) {
        if (Dev->Dfll.Control.prepared || Dev->Dfll.ClocksOwned ||
            Status == STATUS_DEVICE_HARDWARE_ERROR) Status = SwFanDvfsFailureLocked(Dev, Status);
        SwFanCpuUnlock(Dev); return Status;
    }
    Dev->CpuOverclockEnabled = Dev->BaseCpuOverclockEnabled;
    SwFanPublishGpuPolicy(Dev, GpuMode, GpuMinKhz, GpuMaxKhz);
    Dev->PowerMode = Mode;
    SwFanCpuUnlock(Dev);
    SwFanTrace(L"PowerMode", Mode);
    return STATUS_SUCCESS;
}

static VOID SwFanGetPowerStatus(PSWFAN_DEVICE Dev, SWFAN_POWER_STATUS *Out)
{
    ULONG Policy;
    BOOLEAN ScreenOff = FALSE;
    UCHAR Code = 0;

    SwFanCpuLock(Dev);
    Policy = (ULONG)InterlockedCompareExchange(&Dev->GpuDvfsPolicy, 0, 0);
    RtlZeroMemory(Out, sizeof(*Out));
    Out->Version = SWFAN_POWER_STATUS_VERSION;
    Out->IsLite = Dev->IsLite ? 1u : 0u;
    Out->CpuConfigured = Dev->CpuConfigured ? 1u : 0u;
    Out->PowerMode = Dev->PowerMode;
    Out->CpuBootKhz = Dev->BootCpuKhz;
    Out->CpuTargetKhz = (Dev->IsLite && Dev->CpuConfigured) ?
        (Dev->CpuOverclockEnabled ? CPU_TARGET_KHZ : Dev->BootCpuKhz) : Dev->Config.CpuKhz;
    if (Dev->PowerMode < SwNvpCount) Out->CpuTargetKhz = Dev->Dfll.Control.requested_khz;
    Out->CpuActualKhz = Dev->Car != NULL ? SwCpuKhz(Dev) : 0u;
    Out->RamActualKhz = Dev->Config.RamActualKhz;
    Out->GpuPolicyMode = TG_GPU_DVFS_POLICY_MODE(Policy);
    Out->GpuPolicyMinKhz = TG_GPU_DVFS_POLICY_MIN_KHZ(Policy);
    Out->GpuPolicyMaxKhz = TG_GPU_DVFS_POLICY_MAX_KHZ(Policy);
    Out->GpuReportedKhz = Dev->GpuReportedKhz;
    Out->GpuReportedUv = Dev->GpuReportedUv;
    Out->GpuStatusWord = Dev->GpuStatusWord;
    Out->TempC = Dev->LastTemp;
    Out->TempValid = Dev->TempValid ? 1u : 0u;
    Out->FanDuty = Dev->Duty;

    if (Dev->IsLite && !Dev->Stopping && Dev->Pmic != NULL) {
        SwFanPmicLock(Dev);
        if (NT_SUCCESS(SwFanReadReg(Dev->Pmic, CPU_M4_VOUT, &Code)))
            Out->CpuMv = 250u + (ULONG)Code * 5u;
        if (NT_SUCCESS(SwFanReadReg(Dev->Pmic, GPU_M1_VOUT_REG, &Code)))
            Out->GpuReportedUv = (250u + (ULONG)Code * 5u) * 1000u;
        SwFanPmicUnlock(Dev);
    }

    if (Dev->BacklightLock != NULL) WdfWaitLockAcquire(Dev->BacklightLock, NULL);
    ScreenOff = Dev->IsLite && Dev->BacklightReady && Dev->DisplayState == 0;
    if (Dev->BacklightLock != NULL) WdfWaitLockRelease(Dev->BacklightLock);

    if (Dev->CpuApplied)    Out->Flags |= SWFAN_POWER_FLAG_CPU_APPLIED;
    if (Dev->CpuThrottled)  Out->Flags |= SWFAN_POWER_FLAG_CPU_THROTTLED;
    if (Dev->GpuBridgeSeen) Out->Flags |= SWFAN_POWER_FLAG_GPU_BRIDGE_SEEN;
    if (SwFanGpuLiveCounter(Dev)) Out->Flags |= SWFAN_POWER_FLAG_LIVE_GPU_COUNTER;
    if (ScreenOff)          Out->Flags |= SWFAN_POWER_FLAG_SCREEN_OFF;
    Out->CpuPolicyMinKhz = Dev->Dfll.CpuMinKhz;
    Out->CpuPolicyMaxKhz = Dev->Dfll.CpuMaxKhz;
    Out->CpuRequestedMaxKhz = Dev->Dfll.CpuRequestedMaxKhz;
    Out->GpuRequestedMaxKhz = Dev->PowerMode < SwNvpCount ?
        sw_nvpmodel_profiles[Dev->PowerMode].gpu_max_khz : 460800u;
    Out->CpuSpeedo = Dev->Dfll.Handoff.CpuSpeedo;
    Out->CpuProcessId = Dev->Dfll.Handoff.CpuProcessId;
    Out->CpuDfllAvailable = SwFanDfllAvailable(Dev);
    Out->CpuDfllActive = Dev->Dfll.Control.active;
    Out->CpuLastError = (ULONG)Dev->Dfll.LastStatus;
    Out->CpuLoadPermille = Dev->Dfll.LoadPermille;
    Out->GpuSampleAgeMs = Dev->GpuSampleTime ?
        (ULONG)((KeQueryInterruptTime() - Dev->GpuSampleTime) / 10000u) : ~0u;
    Out->SocTempC = Dev->SocTempC;
    Out->BoardTempC = Dev->BoardTempC;
    Out->TempSampleAgeMs = Dev->TempSampleTime && Dev->TempValid ?
        (ULONG)((KeQueryInterruptTime() - Dev->TempSampleTime) / 10000u) : ~0u;
    SwFanCpuUnlock(Dev);
}

#endif
