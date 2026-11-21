/* SPDX-License-Identifier: MIT */
#include "adapter.h"
#include "gr_firmware_io.h"
#include "gpu_bc.h"
extern ULONG TgGpuDebugStageLimit;

/* Record per-step status: the caller only sees the last code, and a failing
 * cleanup used to mask the real one. */
static VOID record_adapter_status(PCWSTR name, NTSTATUS status)
{
    ULONG value = (ULONG)status;
    (VOID)RtlWriteRegistryValue(RTL_REGISTRY_SERVICES, L"tegragpurender\\Parameters",
                                name, REG_DWORD, &value, sizeof(value));
}

/* Default 5 (full bring-up) as of 2026-11-21: the P0 frequency ladder and a
 * 40-minute soak proved this path stable (STATUS 20.6), and a driver that does
 * nothing unless someone remembers to set a registry value is the bigger hazard.
 * A failing stage still returns an error rather than hanging (the teardown path
 * is skipped and only the adapter reports it); install-safe.cmd writes
 * HardwareStartLimit=1 for the explicit conservative mode. */
static ULONG hw_start_limit(VOID)
{
    RTL_QUERY_REGISTRY_TABLE table[2]={0};
    ULONG value=5;
    table[0].Flags=RTL_QUERY_REGISTRY_DIRECT;
    table[0].Name=L"HardwareStartLimit";
    table[0].EntryContext=&value;
    table[0].DefaultType=REG_DWORD;
    table[0].DefaultData=&value;
    table[0].DefaultLength=sizeof(value);
    (VOID)RtlQueryRegistryValues(RTL_REGISTRY_SERVICES,
        L"tegragpurender\\Parameters",table,NULL,NULL);
    return value>5u?5u:value;
}


NTSTATUS TgGpuStartPlatformAdapter(TG_GPU_ADAPTER *a)
{
    TG_GPU_FIRMWARE firmware={0};struct tg_gpu_power_io io;UCHAR address=0;
    NTSTATUS status,cleanup;
    PAGED_CODE();
    if(!a || a->State!=TgGpuAdapterReady || !a->EngineReady || !a->QueueReady ||
       a->Platform.Bridge || a->Platform.Resources || a->Hardware.Engine ||
       a->Engine.HardwareOwner || a->Worker.Thread)
        return STATUS_INVALID_DEVICE_STATE;
    TgGpuBc(1, 0);
    status=TgGpuOpenPlatform(&a->Platform,&a->Resources);
    TgGpuBc(2, (ULONG)status);
    if(NT_SUCCESS(status))status=TgGpuPlatformPreflight(&a->Platform,&address);
    TgGpuBc(3, (ULONG)status);
    if(NT_SUCCESS(status))status=TgGpuFirmwareLoad(&firmware);
    TgGpuBc(4, (ULONG)status);
    TgGpuDebugStageLimit=hw_start_limit();
    if(NT_SUCCESS(status) && TgGpuDebugStageLimit) {
        TgGpuBc(5, TgGpuDebugStageLimit);
        io=TgGpuPlatformIo(&a->Platform);
        status=TgGpuStartHardware(&a->Hardware,&a->Engine,&io,address,&firmware.payload);
        TgGpuBc(6, (ULONG)status);
        if(TgGpuDebugStageLimit && !NT_SUCCESS(status)){
            /* Bisection: the teardown path must not run while the hardware is
             * partially started (tg_gpu_execute_stop writes GPU registers that
             * are unreachable while the partition is gated). Keep the first
             * failure recorded and let the device node report it. */
            TgGpuFirmwareFree(&firmware);
            return status;
        }
        if(status==STATUS_PENDING){
            /* Bring-up bisection stopped at the requested stage. The teardown
             * path (Isolate/StopHardware -> tg_gpu_execute_stop) writes GPU
             * aperture registers, so it must not run here. Restore only the
             * PMIC rail: leaving the GPU powered between boots is what turned a
             * failed stage into a WHEA hardware error earlier. */
            struct tg_gpu_power_io pio = TgGpuPlatformIo(&a->Platform);
            TgGpuFirmwareFree(&firmware);
            if(pio.write_pmic != NULL && a->Hardware.Power.owned){
                (VOID)pio.write_pmic(pio.context, a->Hardware.Power.pmic_address,
                    a->Hardware.Power.original_voltage,
                    (a->Hardware.Power.original_enable) != 0);
                a->Hardware.Power.owned=FALSE;
                a->Hardware.Power.ready=FALSE;
            }
            return STATUS_SUCCESS;
        }
        if(NT_SUCCESS(status) && !NT_SUCCESS(a->Platform.IoStatus))status=a->Platform.IoStatus;
    } else if(NT_SUCCESS(status)) {
        /* Gate closed: the platform is open and preflighted, no hardware I/O.
         * DVFS and the worker require a ready hardware block, so stop here. */
        TgGpuFirmwareFree(&firmware);
        return STATUS_SUCCESS;
    }
    TgGpuFirmwareFree(&firmware);
    if(NT_SUCCESS(status))TgGpuDvfsAttach(&a->Hardware,&a->Platform,NULL);
    if(NT_SUCCESS(status))status=TgGpuStartWorker(&a->Worker,&a->Queue,&a->Hardware);
    record_adapter_status(L"StatusWorker", status);
    record_adapter_status(L"WorkerHwReady", a->Hardware.Ready ? 1u : 0u);
    record_adapter_status(L"WorkerUserd", (a->Hardware.Engine && a->Hardware.Engine->UserdBar1Ready) ? 1u : 0u);
    record_adapter_status(L"WorkerPower", (a->Hardware.Power.owned && a->Hardware.Power.ready) ? 1u : 0u);
    record_adapter_status(L"WorkerQueueEngine", (a->Queue.Engine != NULL) ? 1u : 0u);
    if(NT_SUCCESS(status))return STATUS_SUCCESS;
    if(!a->Hardware.Power.owned)TgGpuPlatformAbandonRail(&a->Platform,address);
    cleanup=TgGpuDdiStopDevice(a);
    record_adapter_status(L"StatusCleanup", cleanup);
    return NT_SUCCESS(cleanup)?status:cleanup;
}

