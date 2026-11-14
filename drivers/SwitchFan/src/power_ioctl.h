/* SPDX-License-Identifier: MIT OR GPL-2.0-only */
#ifndef SWITCHFAN_POWER_IOCTL_H
#define SWITCHFAN_POWER_IOCTL_H

#ifdef _KERNEL_MODE
#include <ntddk.h>
#else
#include <stdint.h>
#endif

/* Public \\.\SwitchBacklight power mode & live hardware readback IOCTLs.
 * FILE_DEVICE_UNKNOWN (0x22), METHOD_BUFFERED (0):
 *   0x812 READ  -> (0x22 << 16) | (1 << 14) | (0x812 << 2) = 0x00226048
 *   0x813 WRITE -> (0x22 << 16) | (2 << 14) | (0x813 << 2) = 0x0022A04C
 */
#define IOCTL_SWFAN_GET_POWER_STATUS 0x00226048u
#define IOCTL_SWFAN_SET_POWER_MODE   0x0022A04Cu

#define SWFAN_POWER_STATUS_VERSION 1u

enum SwFanPowerMode {
    SwFanPowerModeDefaultFixed    = 0, /* CPU 1497.6 MHz + GPU Fixed 460.8 MHz (Default) */
    SwFanPowerModeEcoDynamic      = 1, /* CPU ~998.4 MHz (800 mV PLLX) + GPU HighOPT <=460.8 MHz */
    SwFanPowerModeBalancedDynamic = 2, /* CPU 1497.6 MHz + GPU HighOPT 153.6..460.8 MHz */
    SwFanPowerModeConsoleDynamic  = 3, /* CPU 1497.6 MHz + GPU HighOPT 153.6..768.0 MHz [Exp] */
    SwFanPowerModeMaxHighOpt      = 4, /* CPU 1497.6 MHz + GPU HighOPT 153.6..844.8 MHz [Exp] */
    SwFanPowerModeCount           = 5
};

#define SWFAN_POWER_FLAG_CPU_APPLIED     0x01u
#define SWFAN_POWER_FLAG_CPU_THROTTLED   0x02u
#define SWFAN_POWER_FLAG_GPU_BRIDGE_SEEN 0x04u
#define SWFAN_POWER_FLAG_SCREEN_OFF      0x08u

typedef struct _SWFAN_POWER_STATUS {
    unsigned int Version;          /* SWFAN_POWER_STATUS_VERSION (1) */
    unsigned int IsLite;           /* 1 on T210B01 Switch Lite */
    unsigned int CpuConfigured;    /* 1 when MAX77812 M4 + PLLX handoff is active */
    unsigned int PowerMode;        /* enum SwFanPowerMode (0..4) */
    unsigned int CpuTargetKhz;     /* target CPU kHz for current mode */
    unsigned int CpuActualKhz;     /* live CAR CCLK/PLLX readback in kHz */
    unsigned int CpuBootKhz;       /* UEFI boot PLLX safe step (~998400 kHz) */
    unsigned int CpuMv;            /* live MAX77812 M4 readback in mV */
    unsigned int RamActualKhz;     /* UEFI EMC rate in kHz (default 1600000) */
    unsigned int GpuPolicyMode;    /* 0 = Fixed, 1 = HighOPT NAPLL DVFS */
    unsigned int GpuPolicyMinKhz;  /* policy floor in kHz */
    unsigned int GpuPolicyMaxKhz;  /* policy ceiling in kHz */
    unsigned int GpuReportedKhz;   /* live GPCCLK counter kHz reported by KMD */
    unsigned int GpuReportedUv;    /* live GPU M1 uV reported by KMD / PMIC */
    unsigned int GpuStatusWord;    /* TG_GPU_DVFS_STATUS_PACK from KMD */
    unsigned int TempC;            /* max(TMP451 remote, local) in C */
    unsigned int TempValid;        /* 1 when TMP451 readback succeeded */
    unsigned int FanDuty;          /* current PWM duty 0..236 */
    unsigned int Flags;            /* SWFAN_POWER_FLAG_* */
} SWFAN_POWER_STATUS;

#endif
