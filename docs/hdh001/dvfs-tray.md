# T210B01 CPU/GPU DVFS and tray review

This candidate completes the CPU clock path missing from the original Gemini
GPU-only delivery (`13eb977`, `9722c5d`, `52aed8d`). SwitchFan 1.0.0.15 owns
CPU/M4 and the shared SPB connection; TegraGpuRender owns GPU/NAPLL transitions.
The Windows recovery baseline remains CPU 1497.6 / GPU 460.8 / EMC 1600 MHz.
No new driver or firmware has been installed by this side conversation.

## Profiles

IDs 0..6 and their requested caps match the captured Switchroot T210B01
`nvpmodel_t210b01.conf`. ID 7 is a separate Windows recovery profile. These are
caps and floors, not promises that the live clock always equals the cap.

| ID | Profile | CPU min / effective max MHz | GPU min / effective max MHz |
|---|---|---:|---:|
| 0 | Console | 102 / 1963.5 | 76.8 / 844.8 |
| 1 | Handheld | 102 / 1581 | 76.8 / 537.6 |
| 2 | OC_CPU | 102 / 2397 | 76.8 / 844.8 |
| 3 | OC_GPU | 102 / 1963.5 | 76.8 / 1228.8 |
| 4 | OC_All | 102 / 2397 | 76.8 / 1228.8 |
| 5 | Perf_All | 1224 / 1963.5 | 460.8 / 844.8 |
| 6 | Perf_OC_All | 1224 / 2397 | 460.8 / 1228.8 |
| 7 | Windows baseline | fixed 1497.6 | fixed 460.8 |

Switchroot requests 3000000 kHz CPU / 2000000 kHz GPU in its OC profiles. The
actual SLT/HighOPT tables stop at 2397000/1228800 kHz. The captured Noble GPU
list ended at 844800 because its DT capped frequency at 844800 and voltage at
800 mV; it was not the full HighOPT OC table. The status API reports requested
and effective policy caps separately. Thermal/characterization limits can
reduce the actual rate further. RAM is not dynamically clocked by this patch.

## CPU ownership and voltage

`dfll_tables.h` ports AggressiveSLT CVB/LUT math with original GPL-2.0-only and
NVIDIA/CTCaer notices. It uses this chip's fused CPU Speedo/process, not the
reference chip's voltage as a universal constant. The reference Speedo1630,
process0 case matches all 23 captured OPP voltages and all 33 PMIC LUT entries.
SLT voltages are used only with the closed-loop DFLL, never with open-loop PLLX.
The CPU regulator characterization ceiling is 1235 mV; thermal caps/floors
and the high-tune voltage/settle requirement are enforced.

New WinX ACPI resources expose DFLL control, clocks and reset registers. A
separate versioned record at 0xA9FB0100 carries HIDREV/SKU/Speedo/process/PMIC
address; the old performance ABI remains intact. Missing/invalid resources or
fuses make dynamic profiles unavailable. Firmware does not enable DFLL at boot.

All CPU transitions and frequency samples take CpuLock. The DFLL core verifies
actual CPU mux ownership, PMIC delivery and a fresh frequency counter. Returning
to PLLX prepares and verifies the boot PLL while CPU remains on DFLL, holds a
suitable rail, confirms the mux change, then disables/drains DFLL I2C with the
required two clear PENDING samples. A failed transition latches the failure;
periodic polling cannot silently retry it. Explicit baseline selection is needed
to clear a successfully recovered failure. Failed recovery blocks resource release.

The tray sends the busiest core's utilization every 250 ms using the documented
NtQuerySystemInformation processor-performance layout. This is a Windows load
hint controller, not Linux's cpufreq governor or Windows PPM integration. Kernel
code owns frequency, voltage and thermal bounds. Missing hints retain the chosen
profile ceiling; exiting the tray does not turn off thermal protection. Profiles
are not persisted across a driver restart/reboot; the baseline is the boot default.

## GPU contract and telemetry

The complete 16-frequency HighOPT table uses the fused GPU Speedo and thermal
range. Normal profiles retain an 800 mV ceiling; OC entries use the upstream
HighOPT ceiling of 900 mV. An OPP exceeding that ceiling in any thermal range
is unavailable. GPU transitions happen only after the worker proves quiescence:
voltage-up before frequency-up; verified frequency-down before voltage-down.
A PMIC write followed by an error/readback failure is reconciled against the
actual rail; stale cached voltage may not authorize another boost.

Kernel-only bridge opcode 0x108 reports live-counter ABI 0x00020001. Legacy KMD
heartbeats cannot mark old clock data as live. The public power status is version
2 (33 DWORDs); setters require `{Version=2, Mode}`, so old Mode0 four-byte setters
are rejected. Counter timestamps and TMP451 sample age distinguish unavailable
or stale readings. GPU counter sampling is at an idle boundary, at most once per
second; a busy/unavailable sample is not fabricated. TMP451 remote/local are
SoC/board temperatures, not independent CPU and GPU junction sensors.

## Tray and charging

The original charging bands remain 55% -> 50–60%, 85% -> 80–90%, Off -> full.
Unlike Switchroot's HIZ drain behavior, this driver keeps external VBUS connected
and changes charging enable only; OTG/PD ownership is retained. This intentional
difference is not a claim of byte-for-byte Linux charging behavior.

Explorer's TaskbarCreated notification recreates icons. Cursor hide/restore keeps
its crash breadcrumb until recovery succeeds. Restore commands return errors
instead of claiming success when a driver or cursor reset failed. The online
restore script resets policies/settings, not driver binaries or firmware.

## Validation and handoff limits

Passed: ASan/UBSan CPU/DFLL table and transition models, real-thread CPU policy
serialization control, GPU voltage/frequency ordering and partial-PMIC failures,
all 16 GPU OPP transitions, battery/USB-PD controls, screen-off fan/CPU ordering;
ARM64 drivers/KMD/tray builds; x64 tray API controls under isolated Wine.
The original unpatched GPU clock code fails the new partial-write regression.

These are software/model checks. No CPU DFLL, OC voltage, GPU rendering, sleep,
charging-boundary, or long-duration stability result is claimed for hardware.
Wine's real system-cursor self-test returned 11; native Windows cursor acceptance
is still pending. Main-thread review must integrate onto its current display KMD,
sign matching packages, preserve its UMD/ICD/display settings, and test baseline,
Handheld/Console, transitions/thermal recovery, then OC independently. Do not use
a generic build-check INF to replace the main thread's native tgd3d package.

References: Switchroot kernel `2d0059fd3167a8df756de2aa0489d4aa70a9fc15`, nvgpu
`1ae0167d360287ca78f5a2572f0de42594140312`; Noble capture SHA256
`87f3b515db3aed79623142f25bda1eedfd5c168288d7bf944b928fc27c415224`.
Microsoft contracts: [WDF timer serialization](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdftimer/ns-wdftimer-_wdf_timer_config),
[processor times](https://learn.microsoft.com/en-us/windows/win32/api/winternl/nf-winternl-ntquerysysteminformation),
[TaskbarCreated](https://learn.microsoft.com/en-us/windows/win32/shell/taskbar#taskbar-creation-notification).


## Main-review integration corrections (2026-11-14)

Build the GPU-enabled SwitchFan with `MESA_TEGRA` set to the current
mesa-switch-woa/tegra-x1-wddm source directory. The GPU PMIC transport and its
private ABI remain maintained there; the local t214/gpu_pmic.h forwards to it
and implements the Windows CPU/power policy.

The main review adds CpuLock around the cooling snapshot's DFLL readback,
cleans up power notifications even after unsuccessful PLLX restoration, aligns
the diagnostic DriverVersion with INF15, and expires stale power-tray GPU
readings. Returning a failure from ReleaseHardware is not a veto of teardown;
cleanup must not leave callbacks targeting a freed WDF context. CPU source/
voltage is not forcibly changed from that cleanup path.

Local ASan/UBSan DFLL/clock/PMIC/charge/USB controls, screen-off controls and
tray API controls passed; these are not real hardware DVFS or stability results.
The main chat will build/sign the combined package with the current display
source, start native validation in mode7, and preserve exact pre-install files
for recovery. SC7 is outside this delivery.


## First native transition: refused before DFLL activation

The first integrated Fan15 trial successfully reported the version2 power ABI,
Speedo1630/process0, available DFLL resources and fixed CPU1497.6/GPU460.8/RAM1600.
Mode1 was refused with Win32 ERROR_NOT_READY (21); the automatic test stopped at
that first request and verified fixed7 again. No successful dynamic or OC mode
transition is claimed for Fan15.

The adapter had two incorrect clock assumptions: T210 PLLP_OUT0 is the VCO and
ignores BASE.DIVP (OUT2 uses that divider), and WinX's I2C5 parent selector6 is
CLK_M, while the adapter accepted only PLLP. Fan16 derives PLLP_OUT0 correctly
and supports those two actual mux parents without changing shared I2C5 settings.
The version2 private firmware handoff adds ClockMKhz, derived from OSC_CTRL and
SPARE_REG0, so the kernel does not guess a 19.2 MHz rate or read outside its ACPI
resources. Public power IOCTL ABI remains2; fixed policy7 and display code are
unchanged. Unknown mux parents/invalid handoff still fail before ownership.

A DIVP=1 negative control fails on the old adapter. Patched ASan/UBSan controls
cover both Windows CLK_M19.2 and Linux PLLP/5=81.6 MHz parents, the resulting DFLL
I2C divisors and unchanged shared mux. Native transition acceptance is pending.
Reference: Switchroot kernel2d0059fd3167a8df756de2aa0489d4aa70a9fc15
clk-tegra210.c, pll_p_params (TEGRA_PLL_VCO_OUT); WinX ClockLibPrivate.h and
Tegra210 clock.c document the mux and CLK_M divider.


## Fan17: fixed PLLP semantics

Fan16 native mode1 still returned Win32 error21 before its clock-source trace;
fixed7 recovery succeeded and the native display stayed Code0. The pinned
Switchroot clk-pll.c (2d0059fd3167a8df756de2aa0489d4aa70a9fc15,
clk_pll_recalc_rate) treats PLLP with BASE.OVERRIDE clear as fixed408MHz;
its software M/N fields do not determine the rate. Fan17 follows this rule,
checks enable/lock/bypass, and retains VCO rate decoding for software override.
The existing raw clock traces are recorded before rejecting those fields.
No shared PLLP/I2C source is reprogrammed. Firmware handoff remains version2.

The Fan16 adapter fails the fixed-PLLP fixture with unused M/N=0; the corrected
adapter passes under ASan/UBSan along with the software-override/DIVP and
CLK_M/PLLP I2C cases and existing PMIC recovery controls. These are models;
native switching remains pending.


## Fan19: claim only the disabled, reset DFLL handoff

Fan18 located mode1 refusal at the ownership guard (stage2). Native values:
CTRL=0, OUTPUT_CFG=0x50200000, DVCO_RESET=1, RESET_W=0x110037ff,
CLK_ENB_W=0x482880fc, while CPU remained PLLX1497600kHz/1120mV.
The I2C master-enable bit alone was incorrectly treated as an active owner.
Fan19 accepts that disabled/reset case only when forced output is also off.
It inhibits new I2C messages and requires two clear pending reads separated
by at least the upstream 2us minimum before resetting the idle controller.
An active loop, forced voltage output, failed disable or undrained request
still prevents takeover. CPU mux and voltage are unchanged on rejection.

Reference: pinned Switchroot clk-dfll.c dfll_i2c_set_output_enabled and
is_output_i2c_req_pending. The captured-idle fixture fails with Fan18 and
passes with Fan19 under ASan/UBSan; active-owner/pending-request rejection
and exact saved gate/reset restoration are checked. Native acceptance pending.
