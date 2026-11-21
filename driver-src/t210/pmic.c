/* SPDX-License-Identifier: MIT
 * T210 (Erista) MAX77621 rail driver for the tegra-x1-wddm T210 platform layer.
 *
 * The I2C transfer shape mirrors the existing switchpwrbtn driver: an SPB
 * target opened through the resource hub by connection id, a single-byte
 * register read/write and a bounded timeout. The caller serializes rail
 * accesses; this layer only moves bytes and applies sw_max77621_vote().
 */
#ifndef SWT210_PMIC_C
#define SWT210_PMIC_C
#include <ntddk.h>
#include <ntstrsafe.h>
#include <wdf.h>
#include <spb.h>
#include "pmic_max77621.h"

typedef struct _SWT210_PMIC {
    WDFIOTARGET I2c;
    UCHAR Address;             /* MAX77621_CPU_ADDR (0x1B) or MAX77621_GPU_ADDR (0x1C) */
    struct sw_max77621 State;
} SWT210_PMIC, *PSWT210_PMIC;

static bool Swt210PmicRead(void *context, uint8_t reg, uint8_t *value)
{
    PSWT210_PMIC pmic = (PSWT210_PMIC)context;
    /* SPB_TRANSFER_LIST: write the register index, then read one byte. */
    struct { SPB_TRANSFER_LIST List; SPB_TRANSFER_LIST_ENTRY Entries[2]; } sequence;
    WDF_MEMORY_DESCRIPTOR input;
    WDF_REQUEST_SEND_OPTIONS options;
    ULONG_PTR bytes = 0;
    NTSTATUS status;

    if (pmic == NULL || pmic->I2c == NULL || value == NULL) return false;
    SPB_TRANSFER_LIST_INIT(&sequence.List, 2);
    sequence.Entries[0] = SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(SpbTransferDirectionToDevice, 0, &reg, 1);
    sequence.Entries[1] = SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(SpbTransferDirectionFromDevice, 0, value, 1);
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&input, &sequence, sizeof(sequence));
    WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    options.Timeout = WDF_REL_TIMEOUT_IN_MS(200);
    status = WdfIoTargetSendIoctlSynchronously(pmic->I2c, NULL, IOCTL_SPB_EXECUTE_SEQUENCE,
                                               &input, NULL, &options, &bytes);
    return NT_SUCCESS(status) && bytes == 2;
}

static bool Swt210PmicWrite(void *context, uint8_t reg, uint8_t value)
{
    PSWT210_PMIC pmic = (PSWT210_PMIC)context;
    UCHAR bytes[2] = { reg, value };
    WDF_MEMORY_DESCRIPTOR memory;
    WDF_REQUEST_SEND_OPTIONS options;
    ULONG_PTR written = 0;
    NTSTATUS status;

    if (pmic == NULL || pmic->I2c == NULL) return false;
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&memory, bytes, sizeof(bytes));
    WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    options.Timeout = WDF_REL_TIMEOUT_IN_MS(200);
    status = WdfIoTargetSendWriteSynchronously(pmic->I2c, NULL, &memory, NULL, &options, &written);
    return NT_SUCCESS(status) && written == sizeof(bytes);
}

static void Swt210PmicSettle(void *context)
{
    LARGE_INTEGER delay;
    UNREFERENCED_PARAMETER(context);
    delay.QuadPart = -10000;    /* 1 ms after a verified VOUT write */
    (VOID)KeDelayExecutionThread(KernelMode, FALSE, &delay);
}

/* Open the MAX77621 I2C target by its resource-hub connection id. */
static BOOLEAN Swt210PmicOpen(WDFDEVICE Device, LARGE_INTEGER Id, UCHAR Address, PSWT210_PMIC Pmic)
{
    WCHAR PathBuffer[64];
    UNICODE_STRING Path;
    WDF_IO_TARGET_OPEN_PARAMS Open;
    NTSTATUS Status;

    if (Pmic == NULL || Pmic->I2c != NULL) return FALSE;
    RtlInitEmptyUnicodeString(&Path, PathBuffer, sizeof(PathBuffer));
    Status = RtlUnicodeStringPrintf(&Path, L"\\Device\\RESOURCE_HUB\\%016I64x", (ULONG64)Id.QuadPart);
    if (!NT_SUCCESS(Status)) return FALSE;
    Status = WdfIoTargetCreate(Device, WDF_NO_OBJECT_ATTRIBUTES, &Pmic->I2c);
    if (!NT_SUCCESS(Status)) { Pmic->I2c = NULL; return FALSE; }
    WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&Open, &Path, GENERIC_READ | GENERIC_WRITE);
    Status = WdfIoTargetOpen(Pmic->I2c, &Open);
    if (!NT_SUCCESS(Status)) {
        WdfObjectDelete(Pmic->I2c);
        Pmic->I2c = NULL;
        return FALSE;
    }
    Pmic->Address = Address;
    return TRUE;
}

/* Vote a minimum rail voltage; minimum_uv/max_uv are in microvolts. */
static BOOLEAN Swt210PmicVote(PSWT210_PMIC Pmic, BOOLEAN Set, ULONG MinimumUv, ULONG MaxUv)
{
    struct sw_max77621_io io;
    if (Pmic == NULL || Pmic->I2c == NULL) return FALSE;
    io.context = Pmic;
    io.read = Swt210PmicRead;
    io.write = Swt210PmicWrite;
    io.settle = Swt210PmicSettle;
    return sw_max77621_vote(&Pmic->State, &io, Set != FALSE, MinimumUv, MaxUv) ? TRUE : FALSE;
}

static VOID Swt210PmicClose(PSWT210_PMIC Pmic)
{
    if (Pmic != NULL && Pmic->I2c != NULL) {
        WdfIoTargetClose(Pmic->I2c);
        WdfObjectDelete(Pmic->I2c);
        Pmic->I2c = NULL;
    }
}
#endif

