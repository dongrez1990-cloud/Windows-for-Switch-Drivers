# C:\SwitchWOA\wifi-rollback.ps1
# Guarded rollback: install the 0.7.9.0 repack (which carries the byte-identical
# 0.7.0.0 binary) unless C:\SwitchWOA\wifi-073-ok exists.
# Runs as SYSTEM from a scheduled task, so it works with no network at all -
# that is the point: it is the escape hatch if 0.7.3.0 does not bring WLAN up.
$ErrorActionPreference = 'Continue'
$log = 'C:\SwitchWOA\wifi-rollback.log'
function Say($m) { "[{0}] {1}" -f (Get-Date -Format 'HH:mm:ss'), $m | Add-Content -Path $log }

if (Test-Path 'C:\SwitchWOA\wifi-073-ok') {
    Say "marker wifi-073-ok present -> no rollback"
    exit 0
}

Say "no ok marker -> rolling back to 0.7.9.0 (0.7.0.0 binary)"
$inf = 'C:\SwitchWOA\drivers\switchwifi-rollback-079\switchwifi.inf'
if (-not (Test-Path $inf)) { Say "FATAL: rollback package missing at $inf"; exit 1 }

$p = Start-Process -FilePath 'pnputil.exe' -ArgumentList "/add-driver `"$inf`" /install" -Wait -PassThru
Say ("pnputil exit={0}" -f $p.ExitCode)

Start-Sleep -Seconds 5
try {
    Restart-NetAdapter -Name 'WLAN' -Confirm:$false -ErrorAction Stop
    Start-Sleep -Seconds 8
    $a = Get-NetAdapter -Name 'WLAN' -ErrorAction Stop
    Say ("adapter after rollback: status={0}" -f $a.Status)
} catch {
    Say ("adapter restart ERROR: " + $_.Exception.Message)
}
Say "rollback done"
