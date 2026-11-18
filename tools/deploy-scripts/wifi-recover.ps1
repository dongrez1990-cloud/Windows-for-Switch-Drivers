# C:\SwitchWOA\wifi-recover.ps1
# Safety net for remote WiFi work on this Switch: restart the WLAN adapter from a
# SYSTEM scheduled task, because a wedged adapter also takes away the SSH path.
# Does NOT touch driver packages - use wifi-rollback.ps1 for that.
$ErrorActionPreference = 'Continue'
$log = 'C:\SwitchWOA\wifi-recover.log'
function Say($m) { "[{0}] {1}" -f (Get-Date -Format 'HH:mm:ss'), $m | Add-Content -Path $log }
Say "recover start"
try {
    $a = Get-NetAdapter -Name 'WLAN' -ErrorAction Stop
    Say ("before: status={0} speed={1}" -f $a.Status, $a.LinkSpeed)
    Restart-NetAdapter -Name 'WLAN' -Confirm:$false -ErrorAction Stop
    Start-Sleep -Seconds 6
    $a = Get-NetAdapter -Name 'WLAN' -ErrorAction Stop
    Say ("after : status={0} speed={1}" -f $a.Status, $a.LinkSpeed)
} catch {
    Say ("ERROR: " + $_.Exception.Message)
}
Say "recover stop"
