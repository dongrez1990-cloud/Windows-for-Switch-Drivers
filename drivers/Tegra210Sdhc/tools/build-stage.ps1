<#
.SYNOPSIS
  Build one T210 UHS deferred-profile stage of nvtsdhc.sys from this tree.

.EXAMPLE
  .\build-stage.ps1 -Stage 1 -Name Tegra210Sdhc-stage1
#>
param(
  [int]$Stage = 1,
  [string]$Name = "Tegra210Sdhc-stage$Stage",
  [string]$Tree = $PSScriptRoot,
  [string]$Inf = 'D:\paopao\.tmp\takeover\nvtsdhc.inf',
  # 'deferred' = off the boot path (worker stages), 'sdport' = advertise UHS and
  # let SDPORT drive CMD6/CMD11 during card init (the risky boot-path profile).
  [string]$Profile = 'deferred',
  # raise a bugcheck (with the step's code) at each failure point of the
  # boot-path profile: the only channel that survives an inaccessible boot disk
  [switch]$ProbeStop,
  # stop the machine right after a verified 1.8 V switch (code 0xDA07)
  [switch]$StopAfterSwitch,
  # after the switch, ask the card one CMD6 and stop (0xDA08 ok / 0xDA09 fail)
  # skip the vendor pad auto-cal inside the 1.8 V switch
  [switch]$NoAutocal,
  # control experiment: run the 1.8 V sequence but keep LDO2 at 3.3 V
  [switch]$Fake33V,
  [switch]$PostCheck,
  # post-check after SDPORT programmed the SDR104 timing (0xDA08/0xDA09)
  [switch]$PostCheck2,
  # report the vendor tuning result (0xDA0B ok / 0xDA0C fail)
  [switch]$PostCheck3,
  # rich beacon at SdhcExecuteTuning: reports state in the bugcheck parameters
  [switch]$Beacon,
  # bounce-buffer ADMA32 for 512-byte-block transfers
  [switch]$Dma,
  # leave pad pull-ups untouched during the 1.8 V switch
  [switch]$KeepPulls,
  # run the vendor tuning inside SdhcSetSpeed(SDR104) while the rail is 1.8 V
  [switch]$TuneAtSpeed,
  # advertise 1.8 V but not SDR104 (1.8 V SDR25 experiment)
  [switch]$NoSdr104,
  # reparent SDMMC1 to PLLC4 at slot init and advertise 199.68 MHz
  [switch]$SlotCar,
  # ignore the first 3.3 V request after a 1.8 V switch (keep the rail)
  [switch]$Hold18V,
  # compile UHS/DMA support but advertise no UHS (stay at 3.3 V HS)
  [switch]$NoAdvertise,
  # inline the buffer data port loop (PIO throughput)
  [switch]$FastPio,
  # extra -D defines (e.g. -Extra 'NV_T210_CAR_DIVISOR=0')
  [string[]]$Extra = @(),
  # blocks moved per miniport PIO call (0 = leave the source default)
  [int]$PollBlocks = 0,
  # SDMMC1 CAR divisor override (0 = 204 MHz); -1 leaves the source default
  [int]$CarDivisor = -1,
  # advertise SDPORT scatter/gather DMA (device-visible addresses) instead of
  # forcing the PIO contract
  [switch]$SgDma,
  [switch]$NoSign
)

$ErrorActionPreference = 'Stop'
$src = Join-Path $Tree 'drivers\Tegra210Sdhc\src\nvtsdhc.c'
$out = Join-Path 'D:\paopao\WoA\Windows-for-Switch-Drivers\build' $Name
$builder = 'D:\paopao\WoA\Windows-for-Switch-Drivers\tools\Build-Driver.ps1'
foreach ($p in $src, $builder, $Inf) { if (-not (Test-Path $p)) { throw "missing: $p" } }

$defines = @(
  'NV_T210_UHS=1',
  'NV_T210_UHS_ACTIVE=1',
  'NV_DIAG_CLOCK=1',
  'NV_DIAG_STOP=0',
  'NV_T210_UHS_NO_CAR=1',
  'NV_T210_UHS_SOFT_FAIL=1'
)
# -SgDma: let SDPORT build scatter/gather lists and use ADMA2 with
# device-visible addresses. Without it the miniport keeps the PIO contract.
if ($SgDma) { $defines += 'NV_T210_PIO_ONLY=0' } else { $defines += 'NV_T210_PIO_ONLY=1' }
if ($Profile -eq 'deferred') {
  $defines += 'NV_T210_UHS_DEFERRED=1'
  $defines += "NV_T210_UHS_STAGE=$Stage"
} elseif ($Profile -ne 'sdport') {
  throw "unknown profile: $Profile"
}
if ($ProbeStop) { $defines += 'NV_T210_UHS_PROBE_STOP=1' }
if ($StopAfterSwitch) { $defines += 'NV_T210_UHS_PROBE_STOP=1'; $defines += 'NV_T210_UHS_STOP_AFTER_SWITCH=1' }
if ($PostCheck) { $defines += 'NV_T210_UHS_PROBE_STOP=1'; $defines += 'NV_T210_UHS_POSTCHECK=1' }
if ($PostCheck2) { $defines += 'NV_T210_UHS_PROBE_STOP=1'; $defines += 'NV_T210_UHS_POSTCHECK2=1' }
if ($PostCheck3) { $defines += 'NV_T210_UHS_PROBE_STOP=1'; $defines += 'NV_T210_UHS_POSTCHECK3=1' }
if ($Beacon) { $defines += 'NV_T210_UHS_BEACON=1' }
if ($Dma) { $defines += 'NV_T210_UHS_DMA=1' }
if ($KeepPulls) { $defines += 'NV_T210_UHS_KEEP_PULLS=1' }
if ($TuneAtSpeed) { $defines += 'NV_T210_UHS_TUNE_AT_SPEED=1' }
if ($NoSdr104) { $defines += 'NV_T210_UHS_NO_SDR104=1' }
if ($SlotCar) { $defines += 'NV_T210_UHS_SLOT_CAR=1' }
if ($Hold18V) { $defines += 'NV_T210_UHS_HOLD_18V=1' }
if ($NoAdvertise) { $defines += 'NV_T210_UHS_NO_ADVERTISE=1' }
if ($FastPio) { $defines += 'NV_T210_FAST_PIO=1' }
if ($PollBlocks -gt 0) { $defines += "NV_PIO_POLL_BLOCKS=$PollBlocks" }
if ($CarDivisor -ge 0) { $defines += "NV_T210_CAR_DIVISOR=$CarDivisor" }
if ($Extra) { $defines += $Extra }
if ($NoAutocal) { $defines += 'NV_T210_UHS_NO_AUTOCAL=1' }
if ($Fake33V) { $defines += 'NV_T210_UHS_FAKE_33V=1' }

Write-Host "profile=$Profile stage=$Stage out=$out"
& $builder -Name 'nvtsdhc' -Sources $src -OutDir $out -Inf $Inf -Wdm -Libs 'sdport.lib' -Defines $defines -NoSign:$NoSign

$sys = Join-Path $out 'nvtsdhc.sys'
if (Test-Path $sys) {
  $i = Get-Item $sys
  $h = (Get-FileHash $sys -Algorithm SHA256).Hash
  "SYS {0} bytes sha {1}" -f $i.Length, $h.Substring(0, 8)
}













