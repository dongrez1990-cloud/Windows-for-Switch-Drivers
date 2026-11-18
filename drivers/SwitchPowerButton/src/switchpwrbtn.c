/*
 * Nintendo Switch power button driver, KMDF + Virtual HID Framework (VHF).
 *
 * ACPI device PWRB (ACPI\SWOA0601):
 *   I2cSerialBus 0x3C on I2C5: MAX77620 PMIC. The power button drives its
 *   EN0 input; there is no GPIO for it on the SoC.
 * Every 50 ms the driver reads (Hekate bdk/power/max77620.h):
 *   ONOFFSTAT 0x15 bit 2 (EN0): 1 while the button is held.
 *   ONOFFIRQ  0x0B (clear on read): bit 3 EN0 rising, bit 2 EN0 falling,
 *     so a press shorter than one poll is still seen.
 * A press reports System Power Down while the display is on, System Wake Up
 * while it is off. This polling profile remains in S0; it does not implement
 * a PMIC/GIC wake interrupt or SC7 entry/resume.
 * Holding the button for several seconds still powers the PMIC off by itself.
 */
#include <ntddk.h>
#include <ntstrsafe.h>
#include <wdf.h>
#include <spb.h>
#include <vhf.h>
#include "button_state.h"

#define MAX77620_ONOFFIRQ    0x0B
#define MAX77620_ONOFFSTAT   0x15
#define ONOFFSTAT_EN0        0x04
#define ONOFFIRQ_EN0_F       0x04
#define ONOFFIRQ_EN0_R       0x08
/* ONOFF_CFG1 (0x41) bit 1 = PWR_OFF, from NintendoSwitchPkg
 * Include/Device/Max77620.h (MAX77620_ONOFFCNFG1_PWR_OFF). That bit is the
 * PMIC's software power-off; ONOFF_CFG2 only carries alarm/RST wake sources. */
#define MAX77620_ONOFFCNFG1     0x41
#define ONOFFCNFG1_PWR_OFF      0x02
#define MAX77620_SD3_VOLT       0x19
#define MAX77620_SD3_CFG        0x20
#define MAX77620_SD_CFG2        0x22
#define MAX77620_LDO1_CFG       0x25
#define MAX77620_LDO2_CFG       0x27
#define MAX77620_LDO6_CFG       0x2F
#define MAX77620_LDO7_CFG       0x31
#define MAX77620_FPS_SD3        0x52
#define SD_CFG_ENABLE           0x08u   /* bit3, as in the proven LDO1 sequence */
#define SD_CFG2_POK             0x08u   /* bit3 of SD3_CFG2 = rail power good */
#define MAX77620_GLBL1          0x00
#define MAX77620_AME_GPIO       0x40
#define POLL_MS              50
#define HOLD_REBOOT_MS       3000
#define HOLD_REBOOT_POLLS    (HOLD_REBOOT_MS / POLL_MS)
#define REPORT_ID_SYSTEM     1

/* System Control: Power Down (0x81), Wake Up (0x83), six padding bits. */
static UCHAR SwPwrReportDescriptor[] = {
    0x05, 0x01, 0x09, 0x80, 0xA1, 0x01,
    0x85, REPORT_ID_SYSTEM,
    0x09, 0x81, 0x09, 0x83, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x02, 0x81, 0x02,
    0x95, 0x06, 0x81, 0x01,
    0xC0
};

typedef struct _SWPWR_DEVICE {
    LARGE_INTEGER I2cId;
    WDFIOTARGET I2c;
    WDFTIMER Timer;
    VHFHANDLE Vhf;
    volatile BOOLEAN Stopping;
    struct swpwr_button_state Button;
    KSPIN_LOCK TimerLock;
    PVOID DisplayCallback;
    volatile LONG DisplayOff;
    UCHAR Report[2];
    ULONG Failures;
    LONG Polls;
    LONG Presses;
    LONG Reports;
    LONG WakeReports, PowerReports;
    ULONG HoldPolls;            /* consecutive polls with EN0 held */
    BOOLEAN HoldFired;
    LONG Rebooting;
    LONG PmicOff;
    ULONG LastPowerAction;
} SWPWR_DEVICE, *PSWPWR_DEVICE;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SWPWR_DEVICE, SwPwrGetDevice)

typedef struct _SWPWR_TIMER {
    WDFDEVICE Device;
} SWPWR_TIMER, *PSWPWR_TIMER;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SWPWR_TIMER, SwPwrGetTimer)

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD SwPwrEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE SwPwrEvtPrepareHardware;
EVT_WDF_DEVICE_SELF_MANAGED_IO_INIT SwPwrEvtSelfManagedIoInit;
EVT_WDF_DEVICE_SELF_MANAGED_IO_CLEANUP SwPwrEvtSelfManagedIoCleanup;
EVT_WDF_DEVICE_SELF_MANAGED_IO_SUSPEND SwPwrEvtSuspend;
EVT_WDF_DEVICE_SELF_MANAGED_IO_RESTART SwPwrEvtRestart;
EVT_WDF_TIMER SwPwrEvtTimer;
EVT_WDF_DEVICE_D0_EXIT SwPwrEvtD0Exit;
EVT_WDF_DEVICE_SELF_MANAGED_IO_FLUSH SwPwrEvtFlush;

static WCHAR SwPwrParamsPath[256];
static const GUID SwPwrDisplayGuid =
    {0x6fe69556,0x704a,0x47a0,{0x8f,0x24,0xc2,0x8d,0x93,0x6f,0xda,0x47}};
static NTSTATUS SwPwrDisplayChanged(LPCGUID Setting, PVOID Value, ULONG Length, PVOID Context)
{
    PSWPWR_DEVICE Dev=Context;
    if (IsEqualGUID(Setting,&SwPwrDisplayGuid) && Length==sizeof(ULONG) && *(PULONG)Value<=2)
        InterlockedExchange(&Dev->DisplayOff,*(PULONG)Value==0);
    return STATUS_SUCCESS;
}

static VOID SwPwrTrace(PCWSTR Name, ULONG Value)
{
    if (SwPwrParamsPath[0] != L'\0' && KeGetCurrentIrql() == PASSIVE_LEVEL) {
        (VOID)RtlWriteRegistryValue(RTL_REGISTRY_ABSOLUTE, SwPwrParamsPath, Name, REG_DWORD, &Value, sizeof(Value));
    }
}

/* ------------------------------------------------------------------- I2C */

static NTSTATUS SwPwrOpenTarget(WDFDEVICE Device, LARGE_INTEGER Id, WDFIOTARGET *Target)
{
    WCHAR PathBuffer[64];
    UNICODE_STRING Path;
    WDF_IO_TARGET_OPEN_PARAMS Open;
    NTSTATUS Status;

    RtlInitEmptyUnicodeString(&Path, PathBuffer, sizeof(PathBuffer));
    Status = RtlUnicodeStringPrintf(&Path, L"\\Device\\RESOURCE_HUB\\%016I64x", (ULONG64)Id.QuadPart);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    Status = WdfIoTargetCreate(Device, WDF_NO_OBJECT_ATTRIBUTES, Target);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&Open, &Path, GENERIC_READ | GENERIC_WRITE);
    Status = WdfIoTargetOpen(*Target, &Open);
    if (!NT_SUCCESS(Status)) {
        WdfObjectDelete(*Target);
        *Target = NULL;
    }
    return Status;
}

static NTSTATUS SwPwrReadReg(WDFIOTARGET Target, UCHAR Register, PUCHAR Value)
{
    SPB_TRANSFER_LIST_AND_ENTRIES(2) Sequence;
    PSPB_TRANSFER_LIST_ENTRY Entries = Sequence.List.Transfers;
    WDF_MEMORY_DESCRIPTOR Input;
    WDF_REQUEST_SEND_OPTIONS Options;
    ULONG_PTR Bytes = 0;
    NTSTATUS Status;

    if (Target == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }
    SPB_TRANSFER_LIST_INIT(&Sequence.List, 2);
    Entries[0] = SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(SpbTransferDirectionToDevice, 0, &Register, 1);
    Entries[1] = SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(SpbTransferDirectionFromDevice, 0, Value, 1);
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Input, &Sequence, sizeof(Sequence));
    WDF_REQUEST_SEND_OPTIONS_INIT(&Options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    Options.Timeout = WDF_REL_TIMEOUT_IN_MS(200);
    Status = WdfIoTargetSendIoctlSynchronously(Target, NULL, IOCTL_SPB_EXECUTE_SEQUENCE, &Input, NULL, &Options, &Bytes);
    if (NT_SUCCESS(Status) && Bytes != 2) {
        Status = STATUS_DEVICE_PROTOCOL_ERROR;
    }
    return Status;
}

/* ---------------------------------------------------------------- button */

/* 5.0.0: the console cannot power off through ACPI (the DSDT has no _S5), so
 * "shut down" in Windows only halts the SoC and the rails stay on. At the very
 * end of a clean shutdown (SelfManagedIoFlush, after the volumes were flushed)
 * the PMIC is told to switch itself off, which is exactly what the hardware
 * long-press would do, only with the data already on the card.
 * Also: holding the button for HOLD_REBOOT_MS performs an orderly restart, so
 * the console comes back through hekate instead of being hard-cut by the PMIC. */

/* ntshutdown: SHUTDOWN_ACTION 0 = shutdown, 1 = reboot. Declared here because
 * the public WDK headers do not expose it; ntoskrnl exports it. */
NTSTATUS NtShutdownSystem(ULONG Action);

static BOOLEAN SwPwrParamEnabled(PCWSTR Name, BOOLEAN Default)
{
    RTL_QUERY_REGISTRY_TABLE Table[2];
    ULONG Value = Default ? 1 : 0;

    if (SwPwrParamsPath[0] == L'\0') return Default;
    RtlZeroMemory(Table, sizeof(Table));
    Table[0].Flags = RTL_QUERY_REGISTRY_DIRECT | RTL_QUERY_REGISTRY_TYPECHECK;
    Table[0].Name = (PWSTR)Name;
    Table[0].EntryContext = &Value;
    Table[0].DefaultType = (REG_DWORD << RTL_QUERY_REGISTRY_TYPECHECK_SHIFT) | REG_NONE;
    if (!NT_SUCCESS(RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, SwPwrParamsPath, Table, NULL, NULL))) {
        return Default;
    }
    return Value != 0;
}

static NTSTATUS SwPwrWriteReg(WDFIOTARGET Target, UCHAR Register, UCHAR Value)
{
    UCHAR Buffer[2] = {Register, Value};
    WDF_MEMORY_DESCRIPTOR Memory;
    WDF_REQUEST_SEND_OPTIONS Options;
    ULONG_PTR Bytes = 0;
    NTSTATUS Status;

    if (Target == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Memory, Buffer, sizeof(Buffer));
    WDF_REQUEST_SEND_OPTIONS_INIT(&Options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    Options.Timeout = WDF_REL_TIMEOUT_IN_MS(200);
    Status = WdfIoTargetSendWriteSynchronously(Target, NULL, &Memory, NULL, &Options, &Bytes);
    if (NT_SUCCESS(Status) && Bytes != sizeof(Buffer)) {
        Status = STATUS_DEVICE_PROTOCOL_ERROR;
    }
    return Status;
}

/* PMIC power-off: ONOFF_CFG1 bit 1 (PWR_OFF), written twice like the vendor
 * power-off helper does. Registry Parameters\PowerOffPmic = 0 disables it. */
static VOID SwPwrPmicOff(PSWPWR_DEVICE Dev)
{
    UCHAR Value = 0;
    NTSTATUS Status;

    if (Dev == NULL || Dev->I2c == NULL) return;
    if (!SwPwrParamEnabled(L"PowerOffPmic", TRUE)) {
        SwPwrTrace(L"PmicOffSkipped", 1);
        return;
    }
    Status = SwPwrReadReg(Dev->I2c, MAX77620_ONOFFCNFG1, &Value);
    SwPwrTrace(L"PmicOffRead", NT_SUCCESS(Status) ? Value : (0xFFFF0000u | (ULONG)Status));
    Value = (UCHAR)(Value | ONOFFCNFG1_PWR_OFF);
    Status = SwPwrWriteReg(Dev->I2c, MAX77620_ONOFFCNFG1, Value);
    if (NT_SUCCESS(Status)) {
        Status = SwPwrWriteReg(Dev->I2c, MAX77620_ONOFFCNFG1, Value);
    }
    SwPwrTrace(L"PmicOffStatus", (ULONG)Status);
    if (NT_SUCCESS(Status)) {
        SwPwrTrace(L"PmicOff", (ULONG)InterlockedIncrement(&Dev->PmicOff));
    }
}

/* Audio bring-up aid: the Realtek codec's AVDD/DBVDD come from MAX77620 SD3
 * (L4T: sound { dbvdd/avdd = <&max77620_sd3> }). If those rails are down the
 * codec still answers I2C while every output is silent, which is exactly the
 * symptom we are chasing, so record the rails once at device start. */
static VOID SwPwrPmicDump(PSWPWR_DEVICE Dev)
{
    static const struct { PCWSTR Name; UCHAR Reg; } Map[] = {
        {L"PmicGlbl1", MAX77620_GLBL1},
        {L"PmicSd3Volt", MAX77620_SD3_VOLT},
        {L"PmicSd3Cfg", MAX77620_SD3_CFG},
        {L"PmicSdCfg2", MAX77620_SD_CFG2},
        {L"PmicFpsSd3", MAX77620_FPS_SD3},
        {L"PmicLdo1Cfg", MAX77620_LDO1_CFG},
        {L"PmicLdo2Cfg", MAX77620_LDO2_CFG},
        {L"PmicLdo6Cfg", MAX77620_LDO6_CFG},
        {L"PmicLdo7Cfg", MAX77620_LDO7_CFG},
        {L"PmicOnOffCfg1", MAX77620_ONOFFCNFG1},
        {L"PmicAmeGpio", MAX77620_AME_GPIO},
    };
    ULONG Index;

    for (Index = 0; Index < RTL_NUMBER_OF(Map); ++Index) {
        UCHAR Value = 0;
        NTSTATUS Status = SwPwrReadReg(Dev->I2c, Map[Index].Reg, &Value);
        SwPwrTrace(Map[Index].Name, NT_SUCCESS(Status) ? Value : (0xFFFF0000u | (ULONG)Status));
    }
}

static VOID SwPwrReboot(PSWPWR_DEVICE Dev)
{
    if (InterlockedCompareExchange(&Dev->Rebooting, 1, 0) != 0) return;
    SwPwrTrace(L"HoldReboot", 1);
    SwPwrTrace(L"RebootCall", (ULONG)NtShutdownSystem(1));   /* 1 = restart */
}

static VOID SwPwrReport(PSWPWR_DEVICE Dev, UCHAR Buttons)
{
    HID_XFER_PACKET Packet;
    NTSTATUS Status;

    Dev->Report[0] = REPORT_ID_SYSTEM;
    Dev->Report[1] = Buttons;
    Packet.reportBuffer = Dev->Report;
    Packet.reportBufferLen = sizeof(Dev->Report);
    Packet.reportId = REPORT_ID_SYSTEM;
    Status = VhfReadReportSubmit(Dev->Vhf, &Packet);
    if (NT_SUCCESS(Status)) {
        SwPwrTrace(L"Reports", (ULONG)InterlockedIncrement(&Dev->Reports));
        if (Buttons==SWPWR_WAKE) SwPwrTrace(L"WakeReports",(ULONG)InterlockedIncrement(&Dev->WakeReports));
        if (Buttons==SWPWR_POWER) SwPwrTrace(L"PowerReports",(ULONG)InterlockedIncrement(&Dev->PowerReports));
    } else {
        SwPwrTrace(L"LastReportError", (ULONG)Status);
    }
}

VOID SwPwrEvtTimer(WDFTIMER Timer)
{
    PSWPWR_DEVICE Dev = SwPwrGetDevice(SwPwrGetTimer(Timer)->Device);
    UCHAR Irq = 0, Stat = 0;
    BOOLEAN Held;
    KIRQL Irql;
    NTSTATUS Status;

    if (Dev->Stopping) return;
    Status = SwPwrReadReg(Dev->I2c, MAX77620_ONOFFIRQ, &Irq);
    if (NT_SUCCESS(Status)) {
        Status = SwPwrReadReg(Dev->I2c, MAX77620_ONOFFSTAT, &Stat);
    }
    if (NT_SUCCESS(Status)) {
        Dev->Failures = 0;
        Held = (Stat & ONOFFSTAT_EN0) != 0;
        /* A 3 s hold restarts through the OS path, which flushes the volumes
           first. The PMIC's own ~8 s hard cut stays as the last resort. */
        if (Held && !Dev->Button.suppress_held) {
            if (++Dev->HoldPolls == HOLD_REBOOT_POLLS) {
                SwPwrReboot(Dev);
            }
        } else {
            Dev->HoldPolls = 0;
        }
        struct swpwr_button_event Event=swpwr_button_update(&Dev->Button,Held,
            (Irq&ONOFFIRQ_EN0_R)!=0,InterlockedCompareExchange(&Dev->DisplayOff,0,0)!=0);
        if (Event.emit) {
            if (Event.buttons) SwPwrTrace(L"Presses",(ULONG)InterlockedIncrement(&Dev->Presses));
            SwPwrReport(Dev,Event.buttons);
            if (Event.pulse) SwPwrReport(Dev,0);
        }
        if (Irq != 0) {
            SwPwrTrace(L"LastOnOffIrq", Irq);
        }
    } else if (++Dev->Failures <= 3 || (Dev->Failures % 100) == 0) {
        SwPwrTrace(L"LastI2cError", (ULONG)Status);
        SwPwrTrace(L"Failures", Dev->Failures);
    }
    if ((InterlockedIncrement(&Dev->Polls) % 200) == 0) {
        SwPwrTrace(L"Polls", (ULONG)Dev->Polls);
        SwPwrTrace(L"LastOnOffStat", Stat);
    }
    KeAcquireSpinLock(&Dev->TimerLock,&Irql);
    if (!Dev->Stopping) {
        WdfTimerStart(Timer, WDF_REL_TIMEOUT_IN_MS(POLL_MS));    /* one-shot passive timer, re-armed */
    }
    KeReleaseSpinLock(&Dev->TimerLock,Irql);
}

/* ------------------------------------------------------------------- PnP */

NTSTATUS SwPwrEvtPrepareHardware(WDFDEVICE Device, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    PSWPWR_DEVICE Dev = SwPwrGetDevice(Device);
    ULONG Count = WdfCmResourceListGetCount(Translated), Index;
    UNREFERENCED_PARAMETER(Raw);

    for (Index = 0; Index < Count; ++Index) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR Desc = WdfCmResourceListGetDescriptor(Translated, Index);
        if (Desc != NULL && Desc->Type == CmResourceTypeConnection &&
            Desc->u.Connection.Class == CM_RESOURCE_CONNECTION_CLASS_SERIAL &&
            Desc->u.Connection.Type == CM_RESOURCE_CONNECTION_TYPE_SERIAL_I2C) {
            Dev->I2cId.LowPart = Desc->u.Connection.IdLowPart;
            Dev->I2cId.HighPart = (LONG)Desc->u.Connection.IdHighPart;
            return STATUS_SUCCESS;
        }
    }
    return STATUS_DEVICE_CONFIGURATION_ERROR;
}

NTSTATUS SwPwrEvtSelfManagedIoInit(WDFDEVICE Device)
{
    PSWPWR_DEVICE Dev = SwPwrGetDevice(Device);
    VHF_CONFIG Config;
    UCHAR Stat = 0, Irq = 0;
    NTSTATUS Status;

    if (Dev->I2c == NULL) {
        Status = SwPwrOpenTarget(Device, Dev->I2cId, &Dev->I2c);
        SwPwrTrace(L"OpenI2cStatus", (ULONG)Status);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
    }
    Status = SwPwrReadReg(Dev->I2c, MAX77620_ONOFFSTAT, &Stat);
    SwPwrTrace(L"FirstReadStatus", (ULONG)Status);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    (VOID)SwPwrReadReg(Dev->I2c, MAX77620_ONOFFIRQ, &Irq);    /* drop edges latched before Windows */
    SwPwrTrace(L"OnOffStatAtStart", Stat);
    SwPwrTrace(L"OnOffIrqAtStart", Irq);
    Dev->Button.suppress_held = (Stat & ONOFFSTAT_EN0) != 0;
    SwPwrPmicDump(Dev);

    if (Dev->Vhf == NULL) {
        VHF_CONFIG_INIT(&Config, WdfDeviceWdmGetDeviceObject(Device), sizeof(SwPwrReportDescriptor),
                        SwPwrReportDescriptor);
        Config.VhfClientContext = Dev;
        Config.VendorID = 0x057E;       /* Nintendo */
        Config.ProductID = 0x5601;
        Config.VersionNumber = 0x0004;
        Status = VhfCreate(&Config, &Dev->Vhf);
        SwPwrTrace(L"VhfCreateStatus", (ULONG)Status);
        if (!NT_SUCCESS(Status)) {
            Dev->Vhf = NULL;
            return Status;
        }
        Status = VhfStart(Dev->Vhf);
        SwPwrTrace(L"VhfStartStatus", (ULONG)Status);
        if (!NT_SUCCESS(Status)) {
            VhfDelete(Dev->Vhf, TRUE);
            Dev->Vhf = NULL;
            return Status;
        }
    }
    if (Dev->Timer == NULL) {
        WDF_TIMER_CONFIG TimerConfig;
        WDF_OBJECT_ATTRIBUTES Attributes;
        WDF_TIMER_CONFIG_INIT(&TimerConfig, SwPwrEvtTimer);
        TimerConfig.AutomaticSerialization = FALSE;
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, SWPWR_TIMER);
        Attributes.ParentObject = Device;
        Attributes.ExecutionLevel = WdfExecutionLevelPassive;
        Status = WdfTimerCreate(&TimerConfig, &Attributes, &Dev->Timer);
        SwPwrTrace(L"TimerCreateStatus", (ULONG)Status);
        if (!NT_SUCCESS(Status)) {
            Dev->Timer = NULL;
            return Status;
        }
        SwPwrGetTimer(Dev->Timer)->Device = Device;
    }
    if (Dev->DisplayCallback==NULL) {
        Status=PoRegisterPowerSettingCallback(WdfDeviceWdmGetDeviceObject(Device),
            &SwPwrDisplayGuid,SwPwrDisplayChanged,Dev,&Dev->DisplayCallback);
        SwPwrTrace(L"DisplayCallbackStatus",(ULONG)Status);
        if (!NT_SUCCESS(Status)) { Dev->DisplayCallback=NULL; return Status; }
    }
    return SwPwrEvtRestart(Device);
}

/* The system power action is only valid while the transition runs, so record it
   in D0Exit and reuse it during the shutdown flush. */
NTSTATUS SwPwrEvtD0Exit(WDFDEVICE Device, WDF_POWER_DEVICE_STATE TargetState)
{
    PSWPWR_DEVICE Dev = SwPwrGetDevice(Device);
    UNREFERENCED_PARAMETER(TargetState);
    Dev->LastPowerAction = (ULONG)WdfDeviceGetSystemPowerAction(Device);
    SwPwrTrace(L"PowerAction", Dev->LastPowerAction);
    return STATUS_SUCCESS;
}

/* KMDF calls this once the volumes have been flushed, before the console is
   halted. A real power-down cuts the PMIC here; a restart leaves it alone. */
VOID SwPwrEvtFlush(WDFDEVICE Device)
{
    PSWPWR_DEVICE Dev = SwPwrGetDevice(Device);
    ULONG Action = Dev->LastPowerAction;

    if (Action == 0) Action = (ULONG)WdfDeviceGetSystemPowerAction(Device);
    SwPwrTrace(L"FlushAction", Action);
    if (Action == (ULONG)PowerActionShutdown || Action == (ULONG)PowerActionShutdownOff) {
        SwPwrPmicOff(Dev);
    }
}

NTSTATUS SwPwrEvtSuspend(WDFDEVICE Device)
{    PSWPWR_DEVICE Dev=SwPwrGetDevice(Device);
    KIRQL Irql;
    KeAcquireSpinLock(&Dev->TimerLock,&Irql);
    Dev->Stopping=TRUE;
    KeReleaseSpinLock(&Dev->TimerLock,Irql);
    if (Dev->Timer) WdfTimerStop(Dev->Timer,TRUE);
    if (Dev->Button.held && Dev->Vhf) SwPwrReport(Dev,0);
    Dev->Button.held=false;
    SwPwrTrace(L"Suspended",1);
    return STATUS_SUCCESS;
}
NTSTATUS SwPwrEvtRestart(WDFDEVICE Device)
{
    PSWPWR_DEVICE Dev=SwPwrGetDevice(Device);
    UCHAR Irq=0,Stat=0;
    KIRQL Irql;
    NTSTATUS Status=SwPwrReadReg(Dev->I2c,MAX77620_ONOFFIRQ,&Irq);
    if (NT_SUCCESS(Status)) Status=SwPwrReadReg(Dev->I2c,MAX77620_ONOFFSTAT,&Stat);
    SwPwrTrace(L"ResumeReadStatus",(ULONG)Status);
    if (!NT_SUCCESS(Status)) return Status;
    Dev->Button.held=false;
    Dev->Button.suppress_held=(Stat&ONOFFSTAT_EN0)!=0;
    SwPwrTrace(L"Suspended",0);
    KeAcquireSpinLock(&Dev->TimerLock,&Irql);
    Dev->Stopping=FALSE;
    WdfTimerStart(Dev->Timer,WDF_REL_TIMEOUT_IN_MS(POLL_MS));
    KeReleaseSpinLock(&Dev->TimerLock,Irql);
    return STATUS_SUCCESS;
}

VOID SwPwrEvtSelfManagedIoCleanup(WDFDEVICE Device)
{
    PSWPWR_DEVICE Dev = SwPwrGetDevice(Device);
    (VOID)SwPwrEvtSuspend(Device);
    if (Dev->DisplayCallback) {
        PoUnregisterPowerSettingCallback(Dev->DisplayCallback);
        Dev->DisplayCallback=NULL;
    }
    if (Dev->Vhf != NULL) {
        VhfDelete(Dev->Vhf, TRUE);
        Dev->Vhf = NULL;
    }
    if (Dev->I2c != NULL) {
        WdfIoTargetClose(Dev->I2c);
        WdfObjectDelete(Dev->I2c);
        Dev->I2c = NULL;
    }
}

NTSTATUS SwPwrEvtDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS Pnp;
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDFDEVICE Device;
    NTSTATUS Status;
    UNREFERENCED_PARAMETER(Driver);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&Pnp);
    Pnp.EvtDevicePrepareHardware = SwPwrEvtPrepareHardware;
    Pnp.EvtDeviceSelfManagedIoInit = SwPwrEvtSelfManagedIoInit;
    Pnp.EvtDeviceSelfManagedIoCleanup = SwPwrEvtSelfManagedIoCleanup;
    Pnp.EvtDeviceSelfManagedIoSuspend = SwPwrEvtSuspend;
    Pnp.EvtDeviceSelfManagedIoRestart = SwPwrEvtRestart;
    Pnp.EvtDeviceSelfManagedIoFlush = SwPwrEvtFlush;
    Pnp.EvtDeviceD0Exit = SwPwrEvtD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &Pnp);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, SWPWR_DEVICE);
    Status=WdfDeviceCreate(&DeviceInit,&Attributes,&Device);
    if (NT_SUCCESS(Status)) {
        PSWPWR_DEVICE Dev=SwPwrGetDevice(Device);
        KeInitializeSpinLock(&Dev->TimerLock);
        Dev->Stopping=TRUE;
    }
    return Status;
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG Config;

    if (RegistryPath->Length / sizeof(WCHAR) + 12 < RTL_NUMBER_OF(SwPwrParamsPath)) {
        RtlCopyMemory(SwPwrParamsPath, RegistryPath->Buffer, RegistryPath->Length);
        RtlCopyMemory(&SwPwrParamsPath[RegistryPath->Length / sizeof(WCHAR)], L"\\Parameters", sizeof(L"\\Parameters"));
        (VOID)RtlCreateRegistryKey(RTL_REGISTRY_ABSOLUTE, SwPwrParamsPath);
    }
    SwPwrTrace(L"DriverVersion", 9);
    WDF_DRIVER_CONFIG_INIT(&Config, SwPwrEvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &Config, WDF_NO_HANDLE);
}
