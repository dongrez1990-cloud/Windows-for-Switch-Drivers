<#
.SYNOPSIS
  Wi-Fi driver for the Switch's Windows (Administrator).

  Needs the UEFI Wi-Fi handoff (WifiPrepare.efi succeeded at boot, so the DSDT
  contains WFI0 / ACPI\SWOA0D01) and the board files this console's own radio
  uses: brcmfmac4356A3-pcie.bin and .txt, copied to
  %windir%\System32\drivers\SwitchWOA. Those two files are licensed Broadcom
  firmware and are deliberately not in this repository.

  -Mode Install : pnputil /add-driver switchwifi.inf /install, then report
  -Mode Status  : report WFI0, the driver's own step report, the board files
  -Mode Watch   : wait for the driver's report to change (Ctrl+C to stop)
  -Mode Remove  : remove the driver package

  Log: C:\SwitchWOA-Wifi.txt
#>
param([Parameter(Mandatory = $true)][ValidateSet('Install', 'Status', 'Watch', 'Remove')][string]$Mode)

$ErrorActionPreference = 'Continue'
# Windows PowerShell on Windows 10/11 ARM64 is x86: reach System32 via Sysnative.
$Sys = Join-Path $env:windir 'Sysnative'
if (-not (Test-Path (Join-Path $Sys 'pnputil.exe'))) { $Sys = Join-Path $env:windir 'System32' }
$Pnputil = Join-Path $Sys 'pnputil.exe'
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Log = 'C:\SwitchWOA-Wifi.txt'
$BoardDir = Join-Path $env:windir 'System32\drivers\SwitchWOA'
function Log([string]$s) { Write-Host $s; Add-Content -Path $Log -Value $s -Encoding UTF8 }
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) { Write-Host 'Run this from an elevated (Administrator) prompt.'; exit 1 }

# The package is signed with this test certificate. Trust is per certificate
# thumbprint (LocalMachine Root + TrustedPublisher), and the locally built
# switchwifi.sys, nvtsdhc.sys etc. share this one, which is NOT necessarily the
# same "CN=SwitchWOA Test Driver Signing" that packages from a release zip carry.
# Importing the bundled public key is idempotent and makes the difference moot.
$Cer = Join-Path $Here 'SwitchWOA-Test.cer'
$Thumb = if (Test-Path $Cer) { (New-Object Security.Cryptography.X509Certificates.X509Certificate2 $Cer).Thumbprint } else { $null }

function Install-Certificate {
  if (-not $Thumb) { Log 'certificate: switchwifi.sys is signed but no SwitchWOA-Test.cer is next to this script'; return }
  foreach ($store in 'Root', 'TrustedPublisher') {
    if (Test-Path "Cert:\LocalMachine\$store\$Thumb") { Log "certificate: already in LocalMachine\$store ($Thumb)"; continue }
    & certutil.exe -f -addstore $store $Cer | Out-Null
    Log "certificate: added to LocalMachine\$store (exit=$LASTEXITCODE, $Thumb)"
  }
}

# The driver writes its one-line step report into the device's hardware key
# (INF file [Station.NT.HW]), because a failed start cannot log anywhere else.
function Get-Report {
  $root = 'HKLM:\SYSTEM\CurrentControlSet\Enum\ACPI\SWOA0D01'
  if (-not (Test-Path $root)) { return $null }
  foreach ($inst in Get-ChildItem $root -ErrorAction SilentlyContinue) {
    $p = Get-ItemProperty $inst.PSPath -ErrorAction SilentlyContinue
    if ($p -and $p.PSObject.Properties.Name -contains 'SwitchWifiReport') { return [string]$p.SwitchWifiReport }
  }
  return $null
}

function Show-Status {
  $dev = @(Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue | Where-Object { $_.PNPDeviceID -like 'ACPI\SWOA0D01*' })
  if ($dev.Count -eq 0) {
    Log 'WFI0: NOT FOUND (ACPI\SWOA0D01).'
    Log '  The ACPI device only exists when the UEFI Wi-Fi handoff ran and succeeded:'
    Log '  WifiPrepare.efi prepares PCIe, then BDS patches the WFI0 _STA to 0x0F.'
    Log '  If the firmware was built without SWITCH_WIFI_RUNTIME, no such device is published.'
  }
  foreach ($d in $dev) {
    Log ("device: {0}  name='{1}' status={2} problem={3} service={4}" -f $d.PNPDeviceID, $d.Name, $d.Status, $d.ConfigManagerErrorCode, $d.Service)
  }
  $r = Get-Report
  if ($r) { Log "driver report: $r" } elseif ($dev.Count) { Log 'driver report: (none yet - the driver has not reached image load)' }

  $svc = Get-Service switchwifi -ErrorAction SilentlyContinue
  if ($svc) { Log ("service: switchwifi state={0} start={1}" -f $svc.Status, $svc.StartType) } else { Log 'service: switchwifi NOT INSTALLED' }

  foreach ($f in 'brcmfmac4356A3-pcie.bin', 'brcmfmac4356A3-pcie.txt') {
    $p = Join-Path $BoardDir $f
    if (Test-Path $p) { Log ("board: {0}  {1} bytes" -f $f, (Get-Item $p).Length) } else { Log ("board: {0}  MISSING ({1})" -f $f, $BoardDir) }
  }
  $n = @(Get-NetAdapter -ErrorAction SilentlyContinue | Where-Object { $_.InterfaceDescription -like '*Switch*' -or $_.DriverDescription -like '*Switch*' })
  foreach ($a in $n) { Log ("adapter: '{0}' status={1} link={2}" -f $a.Name, $a.Status, $a.LinkSpeed) }
  if (-not $n.Count -and $dev.Count) { Log 'adapter: none (NetAdapterCx has not published a NIC)' }
}

switch ($Mode) {
  'Install' {
    Log ''
    Log "=== Wi-Fi Install  $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
    Install-Certificate
    $inf = Join-Path $Here 'switchwifi.inf'
    (& $Pnputil /add-driver $inf /install 2>&1) | ForEach-Object { Log "  $_" }
    Log "  exit=$LASTEXITCODE"
    Start-Sleep -Seconds 8
    Show-Status
  }
  'Status' {
    Log ''
    Log "=== Wi-Fi Status  $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
    if ($Thumb) {
      foreach ($store in 'Root', 'TrustedPublisher') {
        Log ("certificate in LocalMachine\{0}: {1}" -f $store, (Test-Path "Cert:\LocalMachine\$store\$Thumb"))
      }
    }
    Show-Status
  }
  'Watch' {
    $last = $null
    while ($true) {
      $r = Get-Report
      if ($r -ne $last) { Write-Host ("{0}  {1}" -f (Get-Date -Format 'HH:mm:ss'), $(if ($r) { $r } else { '(no report)' })); $last = $r }
      Start-Sleep -Seconds 2
    }
  }
  'Remove' {
    $oem = & $Pnputil /enum-drivers 2>&1 | Out-String
    foreach ($x in [regex]::Matches($oem, '(?ms)Published Name\s*:\s*(oem\d+\.inf)\s*\r?\n\s*Original Name\s*:\s*switchwifi\.inf')) {
      $name = $x.Groups[1].Value
      Log "pnputil /delete-driver $name /uninstall /force"
      (& $Pnputil /delete-driver $name /uninstall /force 2>&1) | ForEach-Object { Log "  $_" }
    }
    Show-Status
  }
}
