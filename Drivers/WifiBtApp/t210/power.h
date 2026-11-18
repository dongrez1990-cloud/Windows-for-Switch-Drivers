/* SPDX-License-Identifier: GPL-2.0-only */
/* T210 "Erista" PCIe supply policy.
 *
 * Reference: switchroot tegra210-odin-common.dtsi
 *   avdd-pll-uerefe-supply = <&max77620_ldo7>;   <-- B01 uses ldo1 here
 *   hvddio-pex-supply      = <&max77620_sd3>;
 *   dvddio-pex-supply      = <&max77620_ldo1>;
 *   dvdd-pex-pll-supply    = <&max77620_ldo1>;
 *   hvdd-pex-pll-e-supply  = <&max77620_sd3>;
 *   vddio-pex-ctl-supply   = <&max77620_sd3>;
 *
 * LDO1 and SD3 are the same rails as the B01 profile that has already been
 * accepted on hardware, so the proven T214PcieSupplyEnable()/Restore() sequence
 * is reused rather than duplicated. The single Erista difference is
 * avdd-pll-uerefe on LDO7, which BL31/Hekate normally leaves enabled; this file
 * only OBSERVES it and reports it. That is what separates "the PLL never locked
 * because its reference rail was cold" from a lane/UPHY problem on a first
 * bring-up, without writing a rail whose exact enable sequence has not been
 * verified on this board.
 *
 * MAX77620 register names are from Include/Device/Max77620.h:
 *   LDO7_CFG 0x31, LDO7_CFG2 0x32, FPS_LDO7 0x4D, LDO1_CFG 0x25, FPS_LDO1 0x47.
 * Copyright (c) 2016 NVIDIA CORPORATION. All rights reserved.
 */
#ifndef WIFI_T210_POWER_H
#define WIFI_T210_POWER_H

#include "../t214/power.h"   /* shared LDO1/SD3 policy + PMIC access helpers */

#define MAX77620_LDO7_CFG    0x31
#define MAX77620_LDO7_CFG2   0x32
#define MAX77620_FPS_LDO7    0x4D
#define MAX77620_FPS_ENFPS_SW_MASK 0x01

typedef struct {
  T214_PCIE_SUPPLY Shared;    /* LDO1/SD3, programmed exactly as on B01 */
  UINT8 Ldo7Cfg;              /* observed only, never written */
  UINT8 Ldo7Cfg2;
  UINT8 Ldo7Fps;
} ERISTA_PCIE_SUPPLY;

STATIC BOOLEAN
EristaPcieSupplyEnable (ERISTA_PCIE_SUPPLY *State)
{
  ZeroMem (&State->Shared, sizeof (State->Shared));
  State->Ldo7Cfg = 0xFF;
  State->Ldo7Cfg2 = 0xFF;
  State->Ldo7Fps = 0xFF;

  // Diagnostic only: a read failure must not abort the bring-up, because the
  // rail may legitimately be owned by BL31/Hekate.
  if (T214PciePmicRead (MAX77620_LDO7_CFG, &State->Ldo7Cfg) &&
      T214PciePmicRead (MAX77620_LDO7_CFG2, &State->Ldo7Cfg2) &&
      T214PciePmicRead (MAX77620_FPS_LDO7, &State->Ldo7Fps)) {
    WifiLogF ("wifiprep: supply ldo7 cfg=%02x cfg2=%02x fps=%02x", State->Ldo7Cfg, State->Ldo7Cfg2, State->Ldo7Fps);
    Print (L"PCIe supply: LDO7(avdd-pll-uerefe) cfg=%02x cfg2=%02x fps=%02x%s (Erista)\r\n",
           State->Ldo7Cfg, State->Ldo7Cfg2, State->Ldo7Fps,
           (State->Ldo7Fps & MAX77620_FPS_ENFPS_SW_MASK) ? L" enabled" : L"");
  } else {
    WifiLogF ("wifiprep: supply ldo7 unreadable");
    Print (L"PCIe supply: LDO7 unreadable; continuing with LDO1/SD3 only\r\n");
  }

  // LDO1 (dvddio-pex / dvdd-pex-pll) plus observed SD3 (hvddio-pex /
  // vddio-pex-ctl) are the same rails as B01, so reuse the accepted sequence.
  // A failure here is NOT fatal on Erista: hekate already leaves LDO1/SD3 in
  // the PCIe state, and the shell test (wifibt pex) shows the link trains and
  // the endpoint enumerates without this programming at all. Hiding the WIFI
  // device over a PMIC probe would block the whole Windows path.
  if (!T214PcieSupplyEnable (&State->Shared)) {
    WifiLogF ("wifiprep: supply programming failed; keeping the hekate state");
    (VOID)T214PcieSupplyRestore (&State->Shared);
  } else {
    WifiLogF ("wifiprep: supply programmed (LDO1/SD3)");
  }
  return TRUE;
}

STATIC BOOLEAN
EristaPcieSupplyRestore (ERISTA_PCIE_SUPPLY *State)
{
  // Only LDO1 is ever changed, so the shared restore is the whole restore.
  return T214PcieSupplyRestore (&State->Shared);
}
#endif
