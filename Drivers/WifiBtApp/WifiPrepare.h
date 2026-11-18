/* SPDX-License-Identifier: GPL-2.0-only */
/* Included at the end of WifiBt.c: reuse the physically accepted PCIe path.
 * No host DMA allocation survives this application. Windows loads the radio
 * firmware after it owns the ACPI device, registers and interrupt. */
#include "t214/power.h"
#include "t210/power.h"
#include "../../WindowsDrivers/drivers/SwitchWifi/platform/tegra_resources.h"

EFI_STATUS EFIAPI
WifiPrepareMain (EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
  T214_PCIE_SUPPLY Supply;
  ERISTA_PCIE_SUPPLY ESupply;
  UINT32 AfiAsid = 0, LicClass = 0, LicEnabled = 0;
  BOOLEAN AfiChanged = FALSE, LicChanged = FALSE, Started = FALSE;
  (VOID)ImageHandle;
  (VOID)SystemTable;
  ZeroMem (&Supply, sizeof (Supply));
  ZeroMem (&ESupply, sizeof (ESupply));
  WifiLogF ("==== wifiprep start soc=%08x ====", MmioRead32 (0x70000804UL));
  WifiLogF ("wifiprep: [0] supply (%a)", PexIsB01 () ? "B01" : "T210");
  // Both boards use the MAX77620 on I2C5. B01 programs the Lite-validated
  // LDO1/SD3 profile; Erista uses those same rails plus the LDO7 reference,
  // which is only observed (see t210/power.h). The Erista helper is tolerant:
  // a failed rail probe keeps hekate's state and the bring-up goes on.
  i2c_init (I2C_5);
  if (PexIsB01 ()) {
    if (!T214PcieSupplyEnable (&Supply)) goto Failed;
  } else {
    EristaPcieSupplyEnable (&ESupply);
  }
  Started = TRUE;
  WifiLogF ("wifiprep: [0] supply done");
  Log (L"wifiprep: [1] pex up + enum ...");
  if (!PexUp () || !PexEnum ()) {
    Log (L"wifiprep: [1] PexUp/PexEnum FAILED");
    goto Failed;
  }
  WifiLogF ("wifiprep: [1] pex up + enum OK");
  Log (L"wifiprep: [1] ok, BAR0=%08x size=%08x BAR2=%08x size=%08x",
       (UINT32)mBar0, mBar0Size, (UINT32)mBar2, mBar2Size);

  // PexEnum left memory decoding on and verified that endpoint DMA is off.
  Log (L"wifiprep: [2] PexChip (BAR0 window, chip id, EROM, CR4) ...");
  if (!PexChip ()) {
    Log (L"wifiprep: [2] PexChip FAILED");
    goto Failed;
  }
  Log (L"wifiprep: [2] PexChip ok, halting bus mastering");
  if (!BpWr32 (mPcieCore + 0x4C, 0)) {
    Log (L"wifiprep: [2] bus-master halt write FAILED");
    goto Failed;
  }
  Log (L"wifiprep: [2] ok");
  WifiLogF ("wifiprep: [2] PexChip OK");

  // This platform has no Windows Tegra SMMU/IORT driver. Establish a direct
  // AFI DMA domain while bus mastering is off; Windows uses 32-bit HAL DMA
  // logical addresses and uncached/noncoherent common buffers (_CCA=0).
  // Other SMMU clients are untouched. Failure restores the inherited domain.
  Log (L"wifiprep: [3] AFI DMA domain ...");
  AfiAsid = MmioRead32 (0x70019238UL);
  AfiChanged = TRUE;
  MmioWrite32 (0x70019238UL, 0);
  if (MmioRead32 (0x70019238UL) != 0) {
    Log (L"wifiprep: [3] AFI asid did not clear");
    goto Failed;
  }
  Log (L"wifiprep: [3] ok (asid was %08x)", AfiAsid);
  WifiLogF ("wifiprep: [3] AFI OK");
  // Switchroot irq-tegra.c unmasks the legacy controller as well as GIC.
  // Windows owns the GIC; prepare only SPI 98's CPU route while AFI and the
  // endpoint remain masked. Preserve all other IRQ/FIQ routing bits.
  Log (L"wifiprep: [4] LIC SPI 98 route ...");
  LicClass = MmioRead32 (SWIFI_LIC_BASE + 0x2C);
  LicEnabled = MmioRead32 (SWIFI_LIC_BASE + 0x20);
  LicChanged = TRUE;
  MmioWrite32 (SWIFI_LIC_BASE + 0x2C, LicClass & ~SWIFI_LIC_MASK);
  MmioWrite32 (SWIFI_LIC_BASE + 0x1C, SWIFI_LIC_MASK); // Clear stale forced IRQ.
  MmioWrite32 (SWIFI_LIC_BASE + 0x24, SWIFI_LIC_MASK); // CPU_IER_SET.
  if (!(MmioRead32 (SWIFI_LIC_BASE + 0x20) & SWIFI_LIC_MASK) ||
      (MmioRead32 (SWIFI_LIC_BASE + 0x2C) & SWIFI_LIC_MASK)) {
    Log (L"wifiprep: [4] LIC route readback wrong");
    goto Failed;
  }
  Log (L"wifiprep: [4] ok");
  WifiLogF ("wifiprep: [4] LIC OK");
  MemoryFence ();
  Log (L"Windows Wi-Fi handoff: PCIe ready, CR4 halted, bus mastering/interrupts off");
  Log (L"BAR0=%08x size=%08x BAR2=%08x size=%08x; Windows owns firmware and DMA",
       (UINT32)mBar0, mBar0Size, (UINT32)mBar2, mBar2Size);
  WifiLogF ("wifiprep: SUCCESS, handing to Windows");
  return EFI_SUCCESS; // Keep the LDO1/PHY/link prepared for the Windows owner.

Failed:
  if (Started) PexOff ();
  if (LicChanged) {
    UINT32 Class = MmioRead32 (SWIFI_LIC_BASE + 0x2C);
    MmioWrite32 (SWIFI_LIC_BASE + 0x2C, (Class & ~SWIFI_LIC_MASK) | (LicClass & SWIFI_LIC_MASK));
    MmioWrite32 (SWIFI_LIC_BASE + ((LicEnabled & SWIFI_LIC_MASK) ? 0x24 : 0x28), SWIFI_LIC_MASK);
  }
  if (AfiChanged) {
    MmioWrite32 (0x70019238UL, AfiAsid);
    MemoryFence ();
  }
  if (PexIsB01 () ? Supply.Changed : ESupply.Shared.Changed) {
    MmioWrite32 (0x60006310UL, BIT10);
    MicroSecondDelay (20);
  }
  if (PexIsB01 () ? !T214PcieSupplyRestore (&Supply)
                  : !EristaPcieSupplyRestore (&ESupply))
    Log (L"Windows Wi-Fi preparation: supply restore failed");
  return EFI_DEVICE_ERROR;
}
