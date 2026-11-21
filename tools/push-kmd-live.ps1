# Push a KMD package to the live console over SSH and stage it for the next boot.
#
#   powershell -NoProfile -File push-kmd-live.ps1 [-Version 0.1.0.97] [-Limit 2]
#        [-RgWrite 1] [-ClampNeedsTd 0] [-ApertureProbe 0] [-NoReboot]
#
# The console's SSH account is elevated, so this uses the documented live path:
#   pnputil /add-driver  -> point the device class key at the new oemNN.inf
#   -> ConfigFlags |= 0x20 (reinstall at next boot) -> Parameters switches
# No PnP hot-install and no device restart is performed (both have hung the box).
param(
    [string]$Version       = '0.1.0.97',
    [int]$Limit            = 2,
    [int]$Mask             = 0x80,
    [int]$RgWrite          = 1,
    [int]$ClampNeedsTd     = 0,
    [int]$ApertureProbe    = 0,
    [int]$RailProbe        = 0,
    [int]$ToggleProbe      = 0,
    # 0 = leave Parameters\BootGpuKhz untouched; otherwise the boot GPCPLL point
    # (76800..921600 in 76800 steps) used by the P0 frequency ladder.
    [int]$BootGpuKhz       = 0,
    [string]$PkgDir        = 'D:\paopao\WoA\erista-gpu\kmd-package',
    [string]$RemoteDir     = 'C:\SwitchWOA\gpu-pkg',
    [string]$Ip            = '192.168.1.74',
    [string]$User          = 'woa',
    [string]$Key           = 'D:\paopao\ssh\woa_ed25519',
    [switch]$NoReboot
)
$ErrorActionPreference = 'Stop'
$sso = @('-i', $Key, '-o', 'BatchMode=yes', '-o', 'StrictHostKeyChecking=no', '-o', 'LogLevel=ERROR')

foreach ($f in 'tegragpurender.sys', 'tegragpurender.inf', 'tegragpurender.cat') {
    $src = Join-Path $PkgDir $f
    if (-not (Test-Path $src)) { throw "missing $src" }
    & scp @sso $src ("{0}@{1}:{2}/{3}" -f $User, $Ip, $RemoteDir, $f) 2>&1 | Out-Null
    Write-Host ("pushed " + $f)
}

$remote = @"
`$ErrorActionPreference = 'Continue'
`$ver = '$Version'
`$inf = '$RemoteDir\tegragpurender.inf'
`$out = & pnputil /add-driver `$inf 2>&1
(`$out | Out-String) -split "``n" | Where-Object { `$_ -match 'oem[0-9]+\.inf|failed|error' } | Select-Object -First 3
`$target = `$null
foreach (`$f in Get-ChildItem 'C:\Windows\INF' -Filter 'oem*.inf') {
    `$txt = Get-Content `$f.FullName -ErrorAction SilentlyContinue
    if (-not (`$txt -match 'tegragpurender')) { continue }
    `$line = (`$txt | Select-String 'DriverVer=' | Select-Object -First 1).Line
    if (`$line -and `$line -match [regex]::Escape(`$ver)) { `$target = `$f.Name; break }
}
if (-not `$target) { 'ERROR: package not staged'; exit 1 }
`$cls = 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{F01A9D53-3FF6-48D2-9F97-C8A7004BE10C}\0000'
Set-ItemProperty `$cls InfPath `$target
Set-ItemProperty `$cls DriverVersion `$ver
`$inst = 'HKLM:\SYSTEM\CurrentControlSet\Enum\ACPI\SWOA0F01\0'
`$cf = 0
try { `$cf = [int](Get-ItemProperty `$inst).ConfigFlags } catch { `$cf = 0 }
Set-ItemProperty `$inst ConfigFlags ((`$cf -band (-bnot 0x1)) -bor 0x20) -Type DWord
`$k = 'HKLM:\SYSTEM\CurrentControlSet\Services\tegragpurender\Parameters'
Set-ItemProperty `$k HardwareStartLimit $Limit -Type DWord
Set-ItemProperty `$k PowerSkipMask $Mask -Type DWord
Set-ItemProperty `$k RgWrite $RgWrite -Type DWord
Set-ItemProperty `$k ClampNeedsTd $ClampNeedsTd -Type DWord
Set-ItemProperty `$k ApertureProbe $ApertureProbe -Type DWord
Set-ItemProperty `$k RailProbe $RailProbe -Type DWord
Set-ItemProperty `$k ToggleProbe $ToggleProbe -Type DWord
Set-ItemProperty `$k ClockFirst 0 -Type DWord
Set-ItemProperty `$k RequireTdStatus 0 -Type DWord
`$khz = $BootGpuKhz
if (`$khz -gt 0) { Set-ItemProperty `$k BootGpuKhz `$khz -Type DWord }
'inf=' + `$target + ' flags=0x' + ('{0:X}' -f (Get-ItemProperty `$inst).ConfigFlags) +
  ' Limit=' + (Get-ItemProperty `$k).HardwareStartLimit +
  ' RgWrite=' + (Get-ItemProperty `$k).RgWrite +
  ' ClampNeedsTd=' + (Get-ItemProperty `$k).ClampNeedsTd +
  ' BootGpuKhz=' + (Get-ItemProperty `$k).BootGpuKhz
"@

$b64 = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($remote))
$stage = & ssh @sso ("{0}@{1}" -f $User, $Ip) "powershell -NoProfile -EncodedCommand $b64" 2>&1
$stage | ForEach-Object { Write-Host $_ }
if ($stage -match 'ERROR: package not staged') { throw 'staging failed' }

if ($NoReboot) { Write-Host 'staged only (no reboot requested)'; exit 0 }
Add-Content -Path 'D:\paopao\.tmp\push-kmd-live.log' -Value ('--- {0} version {1} ---' -f (Get-Date -Format 'HH:mm:ss'), $Version)
$stage | Add-Content -Path 'D:\paopao\.tmp\push-kmd-live.log'
& ssh @sso ("{0}@{1}" -f $User, $Ip) 'shutdown /r /t 3 /f' 2>&1 | Out-Null
Write-Host ('reboot requested {0}' -f (Get-Date -Format 'HH:mm:ss'))
