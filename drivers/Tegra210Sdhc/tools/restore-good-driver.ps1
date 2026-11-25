<#
.SYNOPSIS
  Offline: keep nvtsdhc bound, put a known-good nvtsdhc.sys back, and capture
  the Diag evidence of the failed boot first.

  Run on the development PC with the Switch SD card mounted (Windows volume,
  default F:). Self-elevates once.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File D:\paopao\.tmp\uhs-stage1\restore-good-driver.ps1
  powershell -ExecutionPolicy Bypass -File ...\restore-good-driver.ps1 -Volume E: -Source D:\...\nvtsdhc.sys
#>
param(
  [string]$Volume = 'F:',
  [string]$Source = 'D:\paopao\WoA\Windows-for-Switch-Drivers\build\Tegra210Sdhc-blk128\nvtsdhc.sys',
  [string]$LogPath = 'D:\paopao\.tmp\uhs-stage1\restore-good-driver.txt',
  [string]$DiagOut = 'D:\paopao\.tmp\uhs-stage1\offline-diag-bootfail.txt'
)

if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltinRole]::Administrator)) {
    Write-Host 're-launching elevated...'
    Start-Process powershell -Verb RunAs -ArgumentList @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $PSCommandPath,
        '-Volume', $Volume, '-Source', $Source, '-LogPath', $LogPath, '-DiagOut', $DiagOut
    )
    return
}

$ErrorActionPreference = 'Continue'
$log = New-Object System.Collections.Generic.List[string]
$drv = "$Volume\Windows\System32\drivers\nvtsdhc.sys"
$hive = "$Volume\Windows\System32\config\SYSTEM"

$log.Add("restore started $(Get-Date -Format o) volume=$Volume")

if (-not (Test-Path $drv)) { $log.Add("MISSING driver path: $drv (wrong volume letter?)"); $log -join "`n" | Set-Content $LogPath -Encoding UTF8; Get-Content $LogPath; return }
if (-not (Test-Path $Source)) { $log.Add("MISSING source build: $Source"); $log -join "`n" | Set-Content $LogPath -Encoding UTF8; Get-Content $LogPath; return }

# 0) Evidence first: what the failed boot recorded before it died.
#    IMPORTANT: read the hive with reg.exe, never with Get-ItemProperty on a
#    mounted hive - the provider keeps a handle open, which then makes
#    "reg unload" fail and leaves the card un-ejectable.
if (Test-Path $hive) {
    reg unload 'HKLM\SDHREC' 2>&1 | Out-Null
    reg load 'HKLM\SDHREC' $hive 2>&1 | Out-Null
    if ($LASTEXITCODE -eq 0) {
        $lines = New-Object System.Collections.Generic.List[string]
        foreach ($cs in @('ControlSet001', 'ControlSet002')) {
            $key = "HKLM\SDHREC\$cs\Services\nvtsdhc\Parameters\Diag"
            $q = reg query $key 2>&1
            if ($LASTEXITCODE -eq 0) {
                $lines.Add("=== $cs ===")
                foreach ($line in $q) { if ($line -match 'REG_') { $lines.Add($line.Trim()) } }
            }
        }
        $lines -join "`n" | Set-Content $DiagOut -Encoding UTF8
        $log.Add("diag captured -> $DiagOut ($($lines.Count) lines)")
        # The hive must be released or the card cannot be ejected.
        for ($i = 0; $i -lt 8; $i++) {
            $u = reg unload 'HKLM\SDHREC' 2>&1 | ForEach-Object { $_.ToString() }
            if ($LASTEXITCODE -eq 0) { $log.Add('hive unloaded'); break }
            Start-Sleep -Seconds 2
        }
    } else {
        $log.Add('hive load failed (diag not captured)')
    }
}

# 1) Keep the binding, only replace the file.
$before = Get-Item $drv
$log.Add("current: $($before.Length) bytes sha $((Get-FileHash $drv -Algorithm SHA256).Hash.Substring(0,8))")
Copy-Item $drv "$drv.bootfail" -Force
Copy-Item $Source $drv -Force
$after = Get-Item $drv
$log.Add("restored: $($after.Length) bytes sha $((Get-FileHash $drv -Algorithm SHA256).Hash.Substring(0,8))")
$log.Add('expect 32680 bytes sha 41EFF78A (blk128: 51 MHz, 128 blocks/call)')

$log.Add('restore done')
$log -join "`n" | Set-Content $LogPath -Encoding UTF8
Get-Content $LogPath
