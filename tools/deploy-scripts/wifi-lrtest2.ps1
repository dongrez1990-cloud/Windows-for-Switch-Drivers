# C:\SwitchWOA\wifi-lrtest2.ps1
# Acceptance test for SwitchWifi 0.7.4.0.
#
# Contract under test (user's reported defect): after a WiFi disconnect the link
# must come back WITHOUT disable/enable or reboot.
#
# Two things are measured that the 0.7.3.0 run could not distinguish:
#   1. the netsh connect result itself (output is captured, not discarded);
#   2. whether keys are re-installed after the reconnect (kadd) and whether
#      EAPOL frames reach the host (erx) - the 4-way handshake counters.
# The WLAN adapter carries the SSH transport, so this runs detached as SYSTEM.
$ErrorActionPreference = 'Continue'
$log = 'C:\SwitchWOA\wifi-lrtest2.log'
$Profile = 'CMCC-ZouLX-5G'
$Cycles = 5

function Say($m) { "[{0}] {1}" -f (Get-Date -Format 'HH:mm:ss.fff'), $m | Add-Content -Path $log }

function Report {
    $p = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Enum\ACPI\SWOA0D01\0\Device Parameters' `
        -Name SwitchWifiReport -ErrorAction SilentlyContinue).SwitchWifiReport
    if (-not $p) { return '<none>' }
    $f = @{}
    foreach ($kv in ($p -split ' ')) { $x = $kv -split '=', 2; if ($x.Count -eq 2) { $f[$x[0]] = $x[1] } }
    "leave=$($f['leave']) dn=$($f['dn']) join=$($f['join']) scan=$($f['scan']) erx=$($f['erx']) kadd=$($f['kadd']) kfail=$($f['kfail']) ev=$($f['ev'])"
}

function Health {
    $d = Get-PnpDevice -InstanceId 'ACPI\SWOA0D01\0' -ErrorAction SilentlyContinue
    $a = Get-NetAdapter -Name 'WLAN' -ErrorAction SilentlyContinue
    $ssid = ''
    foreach ($l in (netsh wlan show interfaces)) { if ($l -match '^\s*SSID\s*:\s*(.+)$') { $ssid = $Matches[1].Trim() } }
    "device=$($d.Status)/$($d.ProblemCode) adapter=$($a.Status) speed=$($a.LinkSpeed) ssid='$ssid'"
}

Say "=== 0.7.4.0 reconnect test: $Cycles cycles, profile '$Profile' ==="
$ver = (Get-PnpDeviceProperty -InstanceId 'ACPI\SWOA0D01\0' -KeyName DEVPKEY_Device_DriverVersion).Data
Say "driver=$ver"
Say "baseline: $(Health) | $(Report)"

for ($i = 1; $i -le $Cycles; $i++) {
    Say "--- cycle $i ---"
    Say "pre-disconnect : $(Health) | $(Report)"

    $dc = (& netsh wlan disconnect 2>&1) -join ' '
    Say ("  netsh disconnect: " + ($dc -replace '\s+', ' '))
    foreach ($s in 1, 2, 3) { Start-Sleep -Seconds 1; Say "  +${s}s: $(Health) | $(Report)" }

    $co = (& netsh wlan connect name=$Profile 2>&1) -join ' '
    Say ("  netsh connect: " + ($co -replace '\s+', ' '))

    $up = $false
    for ($t = 1; $t -le 15; $t++) {
        Start-Sleep -Seconds 2
        $a = Get-NetAdapter -Name 'WLAN' -ErrorAction SilentlyContinue
        if ($a.Status -eq 'Up') { $up = $true; Say "  reconnected after ~$($t*2)s: $(Health) | $(Report)"; break }
    }
    if (-not $up) { Say "  !! NOT reconnected after 30s: $(Health) | $(Report)" }
}

Say "=== summary ==="
Say "final: $(Health) | $(Report)"
$d = Get-PnpDevice -InstanceId 'ACPI\SWOA0D01\0' -ErrorAction SilentlyContinue
$ok = $d.Status -eq 'OK' -and -not $d.ProblemCode
Say ("VERDICT: device {0}/code{1} -> {2}" -f $d.Status, $d.ProblemCode, $(if ($ok) { 'device healthy (never poisoned)' } else { 'FAIL: needs disable/enable' }))
Say "=== end ==="
'lrtest2 finished' | Set-Content 'C:\SwitchWOA\wifi-lrtest2.done'
