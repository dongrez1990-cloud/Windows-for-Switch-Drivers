/* SPDX-License-Identifier: MIT */
#include "hardware.h"
#include "gpu_bc.h"
/* Bring-up bisection: 0 = hardware start disabled by the caller, 1 = power,
 * 2 = +frontend, 3 = +clock, 4 = +GR boot, 5 = full. Read from
 * Parameters\HardwareStartLimit by platform_adapter.c, whose default is 5 since
 * 2026-11-21 (a failing stage still returns an error instead of blocking boot). */
ULONG TgGpuDebugStageLimit;
static ULONG hw_clock_first(VOID)
{
    RTL_QUERY_REGISTRY_TABLE table[2] = { 0 };
    ULONG value = 0;
    table[0].Flags = RTL_QUERY_REGISTRY_DIRECT;
    table[0].Name = L"ClockFirst";
    table[0].EntryContext = &value;
    table[0].DefaultType = REG_DWORD;
    table[0].DefaultData = &value;
    table[0].DefaultLength = sizeof(value);
    (void)RtlQueryRegistryValues(RTL_REGISTRY_SERVICES,
        L"tegragpurender\\Parameters", table, NULL, NULL);
    return value;
}
static ULONG hw_bisect_mask(VOID)
{
    RTL_QUERY_REGISTRY_TABLE table[2] = { 0 };
    ULONG value = 0;
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
/* P0 frequency ladder (HANDOFF 10/P0-1): Parameters\BootGpuKhz selects the
 * boot GPCPLL point without rebuilding the package. The T210 OPP ladder is
 * 76800..921600 kHz in 76800 kHz steps, so anything outside that grid falls
 * back to the compiled TG_GPU_RENDER_KHZ instead of failing the start. */
static ULONG hw_boot_khz(VOID)
{
    RTL_QUERY_REGISTRY_TABLE table[2] = { 0 };
    ULONG value = 0;
    table[0].Flags = RTL_QUERY_REGISTRY_DIRECT;
    table[0].Name = L"BootGpuKhz";
    table[0].EntryContext = &value;
    table[0].DefaultType = REG_DWORD;
    table[0].DefaultData = &value;
    table[0].DefaultLength = sizeof(value);
    (void)RtlQueryRegistryValues(RTL_REGISTRY_SERVICES,
        L"tegragpurender\\Parameters", table, NULL, NULL);
    if (value < 76800u || value > 921600u || (value % 76800u) != 0u)
        return TG_GPU_RENDER_KHZ;
    return value;
}
static VOID record_gate_stage(ULONG stage)
{
    ULONG v = stage;
    (VOID)RtlWriteRegistryValue(RTL_REGISTRY_SERVICES, L"tegragpurender\\\\Parameters",
        L"HardwareStage", REG_DWORD, &v, sizeof(v));
}

/* The graphics template rejects the golden context unless stage/magic/topology
 * match exactly; record them so a refusal names the offending field. */
static VOID record_gate_value(PCWSTR name, ULONG value)
{
    (VOID)RtlWriteRegistryValue(RTL_REGISTRY_SERVICES, L"tegragpurender\\Parameters",
        name, REG_DWORD, &value, sizeof(value));
}


NTSTATUS TgGpuStartHardware(TG_GPU_HARDWARE *h,TG_GPU_CONTEXT_ENGINE *e,
    const struct tg_gpu_power_io *io,UCHAR address,const struct tg_gpu_gr_payload *firmware)
{
    KIRQL irql;NTSTATUS status;struct tg_gpu_gr_work work;ULONG boot_khz;
    PAGED_CODE();
    if(!h || !e || !io || !io->read32 || !io->write32 || !io->delay_us ||
       !io->read_pmic || !io->write_pmic || !firmware || !e->Commands.Handle ||
       h->Engine || h->Boot.Handle || h->Work.Handle || h->Power.owned || h->Clock.owned)
        return STATUS_INVALID_PARAMETER;
    KeAcquireSpinLock(&e->Lock,&irql);
    if(e->Stopping || e->HardwareOwner || e->Active || e->Notification || e->Creating ||
       e->Channel.referenced || e->Graphics.Memory.Handle) {
        KeReleaseSpinLock(&e->Lock,irql);return STATUS_INVALID_DEVICE_STATE;
    }
    RtlZeroMemory(h,sizeof(*h));h->Engine=e;h->Io=*io;
    e->HardwareOwner=h;e->UserdBar1Ready=FALSE;
    KeReleaseSpinLock(&e->Lock,irql);
    boot_khz=hw_boot_khz();
    h->Status.version=TG_GPU_CONTROL_VERSION;h->Status.target_khz=boot_khz;
    /* The requested point is recorded even when the gate stops before the
     * clock stage, so a ladder run always names the frequency it ran at. */
    record_gate_value(L"BootGpuKhzReq", boot_khz);
    h->Status.clock_result=h->Status.execution_result=UINT32_MAX;
    /* Bootstrap never executes these reports; distinct sentinels are required
     * by the shared arena builder and cannot count as completed work.
     */
    h->Status.expected[0]=0x6f5a3c21;h->Status.expected[1]=~0x6f5a3c21u;
    TgGpuBc(7, 0);
    if (!(hw_bisect_mask() & 0x20u)) {
        status=TgGpuAllocatePrivateMemory(&e->Dxgk,TG_GPU_ARENA_BYTES,0x80000000,0xffffffff,&h->Boot);
        if(!NT_SUCCESS(status))return status;
        status=TgGpuAllocatePrivateMemory(&e->Dxgk,TG_GPU_GR_CHANNEL_WORK_BYTES,0x80000000,0xffffffff,&h->Work);
        if(!NT_SUCCESS(status))return status;
    }
    TgGpuBc(8, 0);
    work=(struct tg_gpu_gr_work){h->Work.CpuAddress,(UINT64)h->Work.SystemPhysicalAddress.QuadPart,h->Work.Bytes};
    TgGpuBc(9, 0);
    h->Status.power_result=tg_gpu_power_up(io,address,&h->Power);h->GpuOwned=h->Power.owned;
    TgGpuBc(10, h->Status.power_result);
    h->Status.stage=100+h->Power.stage;
    if(h->Status.power_result!=TG_GPU_POWER_OK)return STATUS_DEVICE_NOT_READY;
    if(TgGpuDebugStageLimit<2){record_gate_stage(100+h->Status.stage);return STATUS_PENDING;}
    /* Bring-up: on T210 the GPU register block may need the GPCPLL running
     * before the frontend is touched; "ClockFirst" swaps the order. */
    if(hw_clock_first()){
        h->Status.clock_result=tg_gpu_clock_start(io,&h->Power,boot_khz,&h->Clock);
        h->Status.stage=200+h->Clock.stage;
        h->Status.measured_khz=h->Clock.measured_khz;h->Status.rail_uv=h->Clock.rail_uv;
        TgGpuBc(11,(ULONG)h->Status.clock_result);
        if(h->Status.clock_result!=TG_GPU_CLOCK_OK)return STATUS_DEVICE_DATA_ERROR;
    }
    if(!(hw_bisect_mask() & 0x80u) && tg_gpu_frontend_start(io,&h->Power,&h->Status.frontend)!=TG_GPU_FE_OK)return STATUS_DEVICE_DATA_ERROR;
    if(TgGpuDebugStageLimit<3){record_gate_stage(200+h->Clock.stage);return STATUS_PENDING;}
    if(!hw_clock_first())h->Status.clock_result=tg_gpu_clock_start(io,&h->Power,boot_khz,&h->Clock);
    h->Status.stage=200+h->Clock.stage;
    h->Status.measured_khz=h->Clock.measured_khz;h->Status.rail_uv=h->Clock.rail_uv;
    if(h->Status.clock_result!=TG_GPU_CLOCK_OK)return STATUS_DEVICE_DATA_ERROR;
    if(TgGpuDebugStageLimit<4){record_gate_stage(300+h->Execution.stage);return STATUS_PENDING;}
    h->Status.execution_result=tg_gpu_boot_graphics(io,&h->Power,h->Boot.CpuAddress,
        (UINT64)h->Boot.SystemPhysicalAddress.QuadPart,firmware,&work,&h->Execution,&h->Status);
    h->Status.stage=300+h->Execution.stage;
    if(h->Status.execution_result!=TG_GPU_EXEC_OK)return STATUS_DEVICE_DATA_ERROR;
    if(TgGpuDebugStageLimit<5)return STATUS_PENDING;
    /* The retained boot BAR1 root now points at the engine's shared USERD /
     * completion page. HardwareOwner pins Commands until verified isolation.
     */
    h->Status.execution_result=tg_gpu_boot_map_userd(io,&h->Power,h->Boot.CpuAddress,
        (UINT64)h->Boot.SystemPhysicalAddress.QuadPart,
        (UINT64)e->Commands.SystemPhysicalAddress.QuadPart+TG_GPU_COMMAND_USERD,&h->Execution,&h->Status);
    if(h->Status.execution_result!=TG_GPU_EXEC_OK)return STATUS_DEVICE_DATA_ERROR;
    if(TgGpuDebugStageLimit<5)return STATUS_PENDING;
    record_gate_value(L"GrResult", h->Status.graphics.result);
    record_gate_value(L"GrStage", h->Status.graphics.stage);
    record_gate_value(L"GrMagic", h->Status.graphics.image_magic);
    record_gate_value(L"GrChanged", h->Status.graphics.image_changed_words);
    record_gate_value(L"GrGpcs", h->Status.graphics.gpcs);
    record_gate_value(L"GrTpcs", h->Status.graphics.tpcs);
    record_gate_value(L"GrZculls", h->Status.graphics.zculls);
    record_gate_value(L"GrBytes", h->Status.graphics.context_bytes);
    record_gate_value(L"GrSaveMbox", h->Status.graphics.save_mailbox);
    record_gate_value(L"GrBindMbox", h->Status.graphics.bind_mailbox);
    record_gate_value(L"ExecStage", h->Execution.stage);
    record_gate_value(L"ExecBootReady", h->Execution.boot_ready ? 1u : 0u);
    /* Remaining inputs of the template validation that are not part of the
     * golden context itself. */
    record_gate_value(L"GrCmdHandle", e->Commands.Handle ? 1u : 0u);
    record_gate_value(L"GrWorkBytes", (ULONG)work.bytes);
    record_gate_value(L"GrWorkAligned", ((ULONG_PTR)work.cpu & 3u) ? 1u : 0u);
    record_gate_value(L"GrGoldOffset", (ULONG)TG_GPU_GR_GOLD_OFFSET);
    record_gate_value(L"GrImageMax", (ULONG)TG_GPU_GR_IMAGE_MAX_BYTES);
    status=TgGpuInitializeGraphicsTemplate(e,&work,&h->Status.graphics);    if(!NT_SUCCESS(status))return status;
    KeAcquireSpinLock(&e->Lock,&irql);
    if(e->Stopping)status=STATUS_DEVICE_NOT_READY;
    else {
        e->UserdBar1Offset=TG_GPU_BOOT_USERD_BAR1_OFFSET;
        e->UserdBar1Ready=TRUE;h->Ready=TRUE;status=STATUS_SUCCESS;
    }
    KeReleaseSpinLock(&e->Lock,irql);return status;
}
NTSTATUS TgGpuIsolateHardware(TG_GPU_HARDWARE *h)
{
    TG_GPU_CONTEXT_ENGINE *e;KIRQL irql;
    enum tg_gpu_clock_result cr=TG_GPU_CLOCK_OK;
    enum tg_gpu_power_result pr;
    UINT32 clamp,reset;
    PAGED_CODE();
    if(!h)return STATUS_INVALID_PARAMETER;
    e=h->Engine;if(!e)return STATUS_SUCCESS;
    KeAcquireSpinLock(&e->Lock,&irql);
    if(e->HardwareOwner!=h) {KeReleaseSpinLock(&e->Lock,irql);return STATUS_INVALID_DEVICE_STATE;}
    if(e->QueueOwner || e->Active || e->Notification || e->Channel.referenced || e->Creating) {
        KeReleaseSpinLock(&e->Lock,irql);return STATUS_DEVICE_BUSY;
    }
    h->Ready=FALSE;e->UserdBar1Ready=FALSE;
    KeReleaseSpinLock(&e->Lock,irql);
    if(!h->Isolated) {
        if(h->Power.ready) {
            tg_gpu_execute_stop(&h->Io,&h->Execution);
            pr=tg_gpu_power_drain(&h->Io,&h->Power);
            if(pr!=TG_GPU_POWER_OK){h->Status.restore_result=pr;return STATUS_IO_TIMEOUT;}
        }
        if(h->Clock.owned) {
            /* Queue recovery may already have isolated/powered off the GPU. */
            cr=h->Power.owned?tg_gpu_clock_stop(&h->Io,&h->Clock):TG_GPU_CLOCK_READBACK_ERROR;
        }
        pr=tg_gpu_power_down(&h->Io,&h->Power);
        h->Status.restore_result=((UINT32)cr<<16)|pr;
        if(pr!=TG_GPU_POWER_OK || h->Power.owned)return STATUS_DEVICE_DATA_ERROR;
        if(h->GpuOwned) {
            clamp=h->Io.read32(h->Io.context,0x7000e6d4);
            reset=h->Io.read32(h->Io.context,0x6000628c);
            if(clamp==UINT32_MAX || reset==UINT32_MAX || !(clamp&1) || !(reset&0x01000000)) {
                h->Power.owned=true;h->Power.ready=false;return STATUS_DEVICE_DATA_ERROR;
            }
        }
        h->Isolated=TRUE;
        if(h->GpuOwned) {
            KeAcquireSpinLock(&e->Lock,&irql);
            if(!++e->GraphicsGeneration)++e->GraphicsGeneration;
            KeReleaseSpinLock(&e->Lock,irql);
        }
        /* Full reset retires GPU-register/DMA ownership even when restoring
         * a saved clock register failed. Report that error; never retry GPU
         * MMIO after power-off merely to clear a software ownership flag.
         */
        h->Clock.owned=h->Clock.running=false;h->Execution.owned=false;
    }
    return h->Status.restore_result?STATUS_DEVICE_DATA_ERROR:STATUS_SUCCESS;
}
NTSTATUS TgGpuStopHardware(TG_GPU_HARDWARE *h)
{
    TG_GPU_CONTEXT_ENGINE *e;KIRQL irql;NTSTATUS status;
    PAGED_CODE();
    if(!h)return STATUS_INVALID_PARAMETER;
    e=h->Engine;if(!e)return STATUS_SUCCESS;
    status=TgGpuIsolateHardware(h);
    if(!h->Isolated || (status!=STATUS_SUCCESS && status!=STATUS_DEVICE_DATA_ERROR))return status;
    status=TgGpuFreePrivateMemory(&e->Dxgk,&h->Work);
    if(!NT_SUCCESS(status))return status;
    status=TgGpuFreePrivateMemory(&e->Dxgk,&h->Boot);
    if(!NT_SUCCESS(status))return status;
    KeAcquireSpinLock(&e->Lock,&irql);
    e->HardwareOwner=NULL;h->Engine=NULL;
    KeReleaseSpinLock(&e->Lock,irql);
    return h->Status.restore_result?STATUS_DEVICE_DATA_ERROR:STATUS_SUCCESS;
}

