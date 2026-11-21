# Offline GPU bring-up switch setter (run as Administrator, SD card inserted).
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File set-gpu-safe.ps1
#   powershell ... -File set-gpu-safe.ps1 -Limit 1 -Mask 0x1F
#
# Limit 0 = driver parses resources + preflight only, no GPU register writes.
# Mask  0x1F = skip every power sub-step (snapshot-only) when Limit is 1.
param(
    [string]$SystemHive = 'F:\Windows\System32\config\SYSTEM',
    [int]$Limit = 0,
    [int]$Mask  = 0x1F
)

$ErrorActionPreference = 'Stop'
if (-not (Test-Path $SystemHive)) { throw "SYSTEM hive not found: $SystemHive (is the SD card inserted?)" }

& reg.exe load HKLM\OFFSYS $SystemHive | Out-Null
try {
    $sel = (Get-ItemProperty 'HKLM:\OFFSYS\Select' -ErrorAction SilentlyContinue).Current
    if (-not $sel) { $sel = 1 }
    $cs = ('ControlSet{0:D3}' -f [int]$sel)
    $k = 'HKLM:\OFFSYS\' + $cs + '\Services\tegragpurender\Parameters'
    if (-not (Test-Path $k)) { throw "offline Parameters key not found: $k" }
    New-ItemProperty -Path $k -Name HardwareStartLimit -Value $Limit -PropertyType DWord -Force | Out-Null
    New-ItemProperty -Path $k -Name PowerSkipMask      -Value $Mask  -PropertyType DWord -Force | Out-Null
    $p = Get-ItemProperty $k
    'HardwareStartLimit=' + $p.HardwareStartLimit + '  PowerSkipMask=0x' + ('{0:X}' -f $p.PowerSkipMask) + '  (' + $cs + ')'
} finally {
    & reg.exe unload HKLM\OFFSYS | Out-Null
}
'offline set-gpu-safe done'
