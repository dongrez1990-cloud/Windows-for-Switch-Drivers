# C:\SwitchWOA\wifi-lrtest.ps1
# Acceptance test for SwitchWifi 0.7.3.0 (Lite 0.5.7 leave policy port).
#
# Contract under test: after a WiFi disconnect the driver must NOT take the
# endpoint-failure path. The adapter must stay usable (device Status=OK,
# ProblemCode 0) so the next connect succeeds WITHOUT disable/enable or reboot.
#
# The WLAN adapter carries the SSH transport, so this runs detached as SYSTEM
# and logs everything; the operator reconnects afterwards and reads this file.
$ErrorActionPreference = 'Continue'
$log = 'C:\SwitchWOA\wifi-lrtest.log'
$Profile = 'CMCC-ZouLX-5G'
$Cycles = 5

function Say($m) { "[{0}] {1}" -f (Get-Date -Format 'HH:mm:ss.fff'), $m | Add-Content -Path $log }

function Report {
    $p = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Enum\ACPI\SWOA0D01\0\Device Parameters' `
        -Name SwitchWifiReport -ErrorAction SilentlyContinue).SwitchWifiReport
    if (-not $p) { return '<none>' }
    # keep the whole line, but pull the interesting fields to the front
    $f = @{}
    foreach ($kv in ($p -split ' ')) {
        $x = $kv -split '=', 2
        if ($x.Count -eq 2) { $f[$x[0]] = $x[1] }
    }
    "leave=$($f['leave']) dn=$($f['dn']) join=$($f['join']) scan=$($f['scan']) step=$($f['step']) kadd=$($f['kadd']) ev=$($f['ev'])"
}

function Health {
    $d = Get-PnpDevice -InstanceId 'ACPI\SWOA0D01\0' -ErrorAction SilentlyContinue
    $a = Get-NetAdapter -Name 'WLAN' -ErrorAction SilentlyContinue
    $ssid = ''
    foreach ($l in (netsh wlan show interfaces)) {
        if ($l -match '^\s*SSID\s*:\s*(.+)$') { $ssid = $Matches[1].Trim() }
    }
    "device=$($d.Status)/$($d.ProblemCode) adapter=$($a.Status) speed=$($a.LinkSpeed) ssid='$ssid'"
}

Say "=== 0.7.3.0 leave-policy test: $Cycles disconnect/reconnect cycles, profile '$Profile' ==="
$ver = (Get-PnpDeviceProperty -InstanceId 'ACPI\SWOA0D01\0' -KeyName DEVPKEY_Device_DriverVersion).Data
Say "driver=$ver"
Say "baseline: $(Health) | $(Report)"

for ($i = 1; $i -le $Cycles; $i++) {
    Say "--- cycle $i ---"
    Say "pre-disconnect : $(Health) | $(Report)"

    & netsh wlan disconnect | Out-Null
    foreach ($s in 1, 2, 3) {
        Start-Sleep -Seconds 1
        Say "  +${s}s after disconnect: $(Health) | $(Report)"
    }

    & netsh wlan connect name="$Profile" | Out-Null
    $up = $false
    for ($t = 1; $t -le 15; $t++) {
        Start-Sleep -Seconds 2
        $a = Get-NetAdapter -Name 'WLAN' -ErrorAction SilentlyContinue
        if ($a.Status -eq 'Up') {
            $up = $true
            Say "  reconnected after ~$($t*2)s : $(Health) | $(Report)"
            break
        }
    }
    if (-not $up) {
        Say "  !! NOT reconnected after 30s: $(Health) | $(Report)"
        Say "  !! this is the regression the 0.5.7 port is supposed to prevent"
    }
}

Say "=== summary ==="
Say "final: $(Health) | $(Report)"
$d = Get-PnpDevice -InstanceId 'ACPI\SWOA0D01\0' -ErrorAction SilentlyContinue
Say ("VERDICT: device {0}/code{1} after {2} cycles -> {3}" -f $d.Status, $d.ProblemCode, $Cycles,
    $(if ($d.Status -eq 'OK' -and -not $d.ProblemCode) { 'PASS (never poisoned)' } else { 'FAIL (needs disable/enable)' }))
Say "=== end ==="
'lrtest finished' | Set-Content 'C:\SwitchWOA\wifi-lrtest.done'
