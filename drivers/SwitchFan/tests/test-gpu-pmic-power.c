/* Host ASan/UBSan test of SwitchFan MAX77812 SPB serialization (PmicLock),
 * M1 voltage/enable safety bounds, and Power Mode 0..4 transitions/readbacks.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../src/t214/performance.h"

typedef uint32_t ULONG;
typedef uint8_t UCHAR;
typedef uint8_t *PUCHAR;
typedef uintptr_t ULONG_PTR;
typedef int32_t LONG;
typedef int32_t NTSTATUS;
typedef bool BOOLEAN;
typedef unsigned KIRQL;
typedef void VOID;
typedef void *PVOID;
typedef ULONG *PULONG;
typedef void *WDFDEVICE;
typedef void *WDFREQUEST;
typedef void *WDFWAITLOCK;
typedef union { int64_t QuadPart; struct { uint32_t LowPart; int32_t HighPart; }; } LARGE_INTEGER;
typedef struct { void *Buffer; size_t Size; } WDF_MEMORY_DESCRIPTOR;
typedef struct { int64_t Timeout; } WDF_REQUEST_SEND_OPTIONS;

#define TRUE true
#define FALSE false
#define NT_SUCCESS(s) ((s) >= 0)
#define STATUS_SUCCESS                    0
#define STATUS_ACCESS_DENIED             -1
#define STATUS_INVALID_PARAMETER         -2
#define STATUS_NOT_SUPPORTED             -3
#define STATUS_DEVICE_NOT_READY          -4
#define STATUS_DEVICE_CONFIGURATION_ERROR -5
#define STATUS_INVALID_DEVICE_STATE      -6
#define STATUS_DEVICE_DATA_ERROR         -7
#define STATUS_DEVICE_PROTOCOL_ERROR     -8
#define STATUS_IO_TIMEOUT                -9
#define RTL_NUMBER_OF(a) (sizeof(a) / sizeof((a)[0]))
#define WDF_REQUEST_SEND_OPTION_TIMEOUT 1
#define WDF_REL_TIMEOUT_IN_MS(n) (n)
#define WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(m, b, s) (*(m) = (WDF_MEMORY_DESCRIPTOR){ (b), (s) })
#define WDF_REQUEST_SEND_OPTIONS_INIT(o, f) ((void)(f), memset((o), 0, sizeof(*(o))))
#define RtlZeroMemory(p, n) memset((p), 0, (n))
#define KernelMode 0
#define UserMode   1
#define DISPATCH_LEVEL 2

static bool pmic_locked = false;
static bool bl_locked = false;
static unsigned pmic_lock_acquires = 0;
static int requestor_mode = KernelMode;
static bool fail_pmic_write = false;
static UCHAR pmic_regs[256];
static ULONG car_regs[64];
static char order_log[128];
static unsigned order_len = 0;

typedef struct {
    UCHAR *Car;
    UCHAR *Pwm;
    LITE_PERFORMANCE Config;
    void *Pmic;
    WDFWAITLOCK PmicLock;
    WDFWAITLOCK BacklightLock;
    LARGE_INTEGER PmicIds[2];
    ULONG ConnectionCount, BootPllx, BootCpuKhz;
    UCHAR BootVout;
    BOOLEAN IsLite, Stopping, BacklightReady;
    BOOLEAN CpuConfigured, CpuApplied, CpuThrottled, CpuOverclockEnabled;
    BOOLEAN BaseCpuOverclockEnabled, GpuBridgeSeen, TempValid;
    ULONG DisplayState, Duty, LastTemp;
    ULONG PowerMode, PolicySeq;
    volatile LONG GpuDvfsPolicy;
    ULONG GpuReportedKhz, GpuReportedUv, GpuStatusWord;
} SWFAN_DEVICE, *PSWFAN_DEVICE;

struct mock_request {
    void *in;
    size_t in_len;
    void *out;
    size_t out_len;
};

#define SWFAN_HAVE_PMIC_LOCK 1
static void WdfWaitLockAcquire(WDFWAITLOCK l, void *t)
{
    (void)t;
    if (l == (WDFWAITLOCK)1) {
        assert(!pmic_locked);
        pmic_locked = true;
        ++pmic_lock_acquires;
    } else if (l == (WDFWAITLOCK)2) {
        assert(!bl_locked);
        bl_locked = true;
    }
}
static void WdfWaitLockRelease(WDFWAITLOCK l)
{
    if (l == (WDFWAITLOCK)1) {
        assert(pmic_locked);
        pmic_locked = false;
    } else if (l == (WDFWAITLOCK)2) {
        assert(bl_locked);
        bl_locked = false;
    }
}
static VOID SwFanPmicLock(PSWFAN_DEVICE Dev)
{ if (Dev->PmicLock != NULL) WdfWaitLockAcquire(Dev->PmicLock, NULL); }
static VOID SwFanPmicUnlock(PSWFAN_DEVICE Dev)
{ if (Dev->PmicLock != NULL) WdfWaitLockRelease(Dev->PmicLock); }

static PSWFAN_DEVICE SwFanGetDevice(void *d) { return d; }
static void SwFanTrace(const void *n, ULONG v) { (void)n; (void)v; }
static VOID SwFanSetDuty(PSWFAN_DEVICE Dev, ULONG Duty) { Dev->Duty = Duty; }
static NTSTATUS SwFanOpenTarget(void *d, LARGE_INTEGER i, void **t)
{ (void)d; (void)i; *t = (void *)1; return STATUS_SUCCESS; }
static void WdfObjectDelete(void *p) { (void)p; }
static int WdfRequestGetRequestorMode(WDFREQUEST r) { (void)r; return requestor_mode; }
static NTSTATUS WdfRequestRetrieveInputBuffer(WDFREQUEST r, size_t min_len, PVOID *buf, size_t *len)
{
    struct mock_request *mr = r;
    if (mr->in_len < min_len) return STATUS_INVALID_PARAMETER;
    *buf = mr->in;
    if (len) *len = mr->in_len;
    return STATUS_SUCCESS;
}
static NTSTATUS WdfRequestRetrieveOutputBuffer(WDFREQUEST r, size_t min_len, PVOID *buf, size_t *len)
{
    struct mock_request *mr = r;
    if (mr->out_len < min_len) return STATUS_INVALID_PARAMETER;
    *buf = mr->out;
    if (len) *len = mr->out_len;
    return STATUS_SUCCESS;
}
static NTSTATUS SwFanReadReg(void *t, UCHAR reg, UCHAR *v)
{
    assert(t != NULL && pmic_locked);
    *v = pmic_regs[reg];
    return STATUS_SUCCESS;
}
static NTSTATUS WdfIoTargetSendWriteSynchronously(void *t, void *r,
    WDF_MEMORY_DESCRIPTOR *m, void *o, WDF_REQUEST_SEND_OPTIONS *s, ULONG_PTR *n)
{
    UCHAR *b = m->Buffer;
    (void)r; (void)o; (void)s;
    assert(t != NULL && pmic_locked && m->Size == 2);
    if (fail_pmic_write) return STATUS_IO_TIMEOUT;
    pmic_regs[b[0]] = b[1];
    if (order_len < sizeof(order_log)) order_log[order_len++] = 'V';
    *n = 2;
    return STATUS_SUCCESS;
}
static void KeDelayExecutionThread(int m, bool a, LARGE_INTEGER *d) { (void)m; (void)a; (void)d; }
static void KeRaiseIrql(unsigned i, KIRQL *o) { (void)i; *o = 0; }
static void KeLowerIrql(KIRQL i) { (void)i; }
static void KeStallExecutionProcessor(unsigned n) { (void)n; }
static LONG InterlockedExchange(volatile LONG *dst, LONG val)
{ LONG old = *dst; *dst = val; return old; }
static LONG InterlockedCompareExchange(volatile LONG *dst, LONG exch, LONG comp)
{ LONG cur = *dst; if (cur == comp) *dst = exch; return cur; }

#define READ_REGISTER_ULONG(p) (*(p))
static void regwrite(ULONG *p, ULONG v)
{
    if (order_len < sizeof(order_log)) order_log[order_len++] = 'C';
    *p = v;
    if (p == car_regs + 0xe0 / 4 && (v & (1u << 30))) *p |= 1u << 27;
}
#define WRITE_REGISTER_ULONG(p, v) regwrite((p), (v))

#include "../src/t214/clock.h"
#include "../src/t214/gpu_pmic.h"

static NTSTATUS send_gpu_pmic(PSWFAN_DEVICE d, struct tg_gpu_pmic_request *op)
{
    struct mock_request req = { op, sizeof(*op), op, sizeof(*op) };
    ULONG_PTR info = 0;
    NTSTATUS st = SwFanGpuPmic(d, &req, &info);
    assert(!pmic_locked && !bl_locked);
    if (NT_SUCCESS(st)) assert(info == sizeof(*op));
    return st;
}

int main(void)
{
    SWFAN_DEVICE d = {0};
    SWFAN_POWER_STATUS st = {0};
    struct tg_gpu_pmic_request op;
    unsigned locks_before;

    memset(pmic_regs, 0, sizeof(pmic_regs));
    memset(car_regs, 0, sizeof(car_regs));
    pmic_regs[0x06] = 0x54; /* M3 DDR + M4 CPU enabled, M1 GPU initially off */
    pmic_regs[0x23] = 20;   /* pre-acquisition cold M1 code (<68 while off) */
    pmic_regs[0x26] = 110;  /* 800 mV boot CPU M4 */
    car_regs[0x20 / 4] = 0x20008888u;
    car_regs[0xe0 / 4] = (1u << 30) | (1u << 27) | (2u << 20) | (156u << 8) | 2u; /* 998.4 MHz */
    car_regs[0xa0 / 4] = 0x50000000u;

    d.IsLite = TRUE;
    d.BacklightReady = TRUE;
    d.DisplayState = 1;
    d.Car = (UCHAR *)car_regs;
    d.Pwm = (UCHAR *)&car_regs[60];
    d.PmicLock = (WDFWAITLOCK)1;
    d.BacklightLock = (WDFWAITLOCK)2;
    d.Config.CpuKhz = LITE_CPU_KHZ;
    d.Config.RamActualKhz = 1600000u;
    d.Config.Status = LitePerfReady;
    d.Config.PmicAddress = 0x33;
    d.ConnectionCount = 3;
    d.TempValid = TRUE;
    d.LastTemp = 41;
    d.Duty = 51;
    d.CpuOverclockEnabled = TRUE;
    d.BaseCpuOverclockEnabled = TRUE;
    d.GpuDvfsPolicy = (LONG)TG_GPU_DVFS_POLICY_PACK(0, 460800u, 460800u, 0);

    SwCpuPrepare(&d);
    assert(d.CpuConfigured && d.BootCpuKhz == 998400u && !pmic_locked);
    SwCpuTemperature(&d, 41, TRUE);
    assert(d.CpuApplied && SwCpuKhz(&d) == 1497600u && pmic_regs[0x26] == 174);

    /* 1. UserMode callers are blocked from TG_GPU_IOCTL_PMIC. */
    requestor_mode = UserMode;
    op = (struct tg_gpu_pmic_request){ 0, TG_GPU_PMIC_QUERY_ADDRESS, 0, 0 };
    assert(send_gpu_pmic(&d, &op) == STATUS_ACCESS_DENIED);
    requestor_mode = KernelMode;

    /* 2. M1 enable requires valid M1_VOUT >= 590 mV (code >= 68); RMW preserves 0x54. */
    op = (struct tg_gpu_pmic_request){ 0x33, 0x06, 1, 1 };
    assert(send_gpu_pmic(&d, &op) == STATUS_INVALID_DEVICE_STATE);
    op = (struct tg_gpu_pmic_request){ 0x33, 0x23, 1, 110 }; /* 800 mV while off */
    assert(send_gpu_pmic(&d, &op) == STATUS_SUCCESS && pmic_regs[0x23] == 110);
    op = (struct tg_gpu_pmic_request){ 0x33, 0x06, 1, 1 };
    assert(send_gpu_pmic(&d, &op) == STATUS_SUCCESS && pmic_regs[0x06] == 0x55);

    /* 3. Live M1 bounds: 68..160 (590..1050 mV) allowed; <68 rejected while M1 on! */
    locks_before = pmic_lock_acquires;
    op = (struct tg_gpu_pmic_request){ 0x33, 0x23, 1, 68 };
    assert(send_gpu_pmic(&d, &op) == STATUS_SUCCESS && pmic_regs[0x23] == 68);
    assert(pmic_lock_acquires == locks_before + 1);
    op = (struct tg_gpu_pmic_request){ 0x33, 0x23, 1, 67 };
    assert(send_gpu_pmic(&d, &op) == STATUS_INVALID_PARAMETER && pmic_regs[0x23] == 68);
    op = (struct tg_gpu_pmic_request){ 0x33, 0x23, 1, 161 };
    assert(send_gpu_pmic(&d, &op) == STATUS_INVALID_PARAMETER && pmic_regs[0x23] == 68);
    /* CPU M4 (0x26) is read-only on GPU bridge; DDR M3 is completely rejected. */
    op = (struct tg_gpu_pmic_request){ 0x33, 0x26, 1, 110 };
    assert(send_gpu_pmic(&d, &op) == STATUS_INVALID_PARAMETER);
    op = (struct tg_gpu_pmic_request){ 0x33, 0x25, 0, 0 };
    assert(send_gpu_pmic(&d, &op) == STATUS_INVALID_PARAMETER);

    /* Disabling M1 allows restoring cold pre-acquisition voltage (<68), preserving 0x54! */
    op = (struct tg_gpu_pmic_request){ 0x33, 0x06, 1, 0 };
    assert(send_gpu_pmic(&d, &op) == STATUS_SUCCESS && pmic_regs[0x06] == 0x54);
    op = (struct tg_gpu_pmic_request){ 0x33, 0x23, 1, 20 };
    assert(send_gpu_pmic(&d, &op) == STATUS_SUCCESS && pmic_regs[0x23] == 20);
    /* Re-enable at 800 mV for runtime tests. */
    op = (struct tg_gpu_pmic_request){ 0x33, 0x23, 1, 110 };
    assert(send_gpu_pmic(&d, &op) == STATUS_SUCCESS);
    op = (struct tg_gpu_pmic_request){ 0x33, 0x06, 1, 1 };
    assert(send_gpu_pmic(&d, &op) == STATUS_SUCCESS);

    /* 4. Power Modes 0..4 + live hardware readbacks. */
    SwFanGetPowerStatus(&d, &st);
    assert(st.PowerMode == SwFanPowerModeDefaultFixed && st.CpuActualKhz == 1497600u);
    assert(st.CpuMv == 1120u && st.GpuPolicyMode == 0 && st.GpuPolicyMaxKhz == 460800u);

    /* Mode 1 (Eco): F-down to 998.4 MHz BEFORE V-down to 800 mV! */
    order_len = 0;
    assert(SwFanSetPowerMode(&d, SwFanPowerModeEcoDynamic) == STATUS_SUCCESS);
    assert(!d.CpuApplied && SwCpuKhz(&d) == 998400u && pmic_regs[0x26] == 110);
    assert(order_len >= 2 && order_log[0] == 'C' && order_log[order_len - 1] == 'V');

    /* Mode 3 (Console Dynamic 768 MHz): V-up to 1120 mV BEFORE F-up to 1497.6 MHz! */
    order_len = 0;
    assert(SwFanSetPowerMode(&d, SwFanPowerModeConsoleDynamic) == STATUS_SUCCESS);
    assert(d.CpuApplied && SwCpuKhz(&d) == 1497600u && pmic_regs[0x26] == 174);
    assert(order_len >= 2 && order_log[0] == 'V' && order_log[1] == 'C');
    op = (struct tg_gpu_pmic_request){ 0, TG_GPU_PMIC_QUERY_DVFS_POLICY, 0, 0 };
    assert(send_gpu_pmic(&d, &op) == STATUS_SUCCESS);
    assert(TG_GPU_DVFS_POLICY_MODE(op.value) == 1 &&
           TG_GPU_DVFS_POLICY_MIN_KHZ(op.value) == 153600u &&
           TG_GPU_DVFS_POLICY_MAX_KHZ(op.value) == 768000u);

    /* Mode 4 (Max HighOPT 844.8 MHz) + KMD live telemetry report -> GetPowerStatus. */
    assert(SwFanSetPowerMode(&d, SwFanPowerModeMaxHighOpt) == STATUS_SUCCESS);
    op = (struct tg_gpu_pmic_request){ 0, TG_GPU_PMIC_REPORT_GPU_KHZ, 1, 844800u };
    assert(send_gpu_pmic(&d, &op) == STATUS_SUCCESS);
    op = (struct tg_gpu_pmic_request){ 0, TG_GPU_PMIC_REPORT_GPU_UV, 1, 610000u };
    assert(send_gpu_pmic(&d, &op) == STATUS_SUCCESS);
    SwFanGetPowerStatus(&d, &st);
    assert(st.PowerMode == SwFanPowerModeMaxHighOpt && st.GpuPolicyMaxKhz == 844800u);
    assert(st.GpuReportedKhz == 844800u && st.GpuReportedUv == 610000u);
    assert((st.Flags & SWFAN_POWER_FLAG_GPU_BRIDGE_SEEN) != 0);

    /* Failure propagation: I2C timeout during Eco downvolt returns real error! */
    fail_pmic_write = true;
    assert(SwFanSetPowerMode(&d, SwFanPowerModeEcoDynamic) == STATUS_IO_TIMEOUT);
    fail_pmic_write = false;
    assert(!pmic_locked);

    /* Unsupported / invalid inputs rejected cleanly. */
    assert(SwFanSetPowerMode(&d, 5) == STATUS_INVALID_PARAMETER);
    d.IsLite = FALSE;
    assert(SwFanSetPowerMode(&d, 0) == STATUS_NOT_SUPPORTED);

    puts("PASS: SwitchFan PmicLock serialization, M1 Vmin/Vmax bounds and Power Mode 0..4 readbacks");
    return 0;
}
