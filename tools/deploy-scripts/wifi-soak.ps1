# C:\SwitchWOA\wifi-soak.ps1
# 10-cycle disconnect/reconnect soak for SwitchWifi 0.7.4.0.
# Upstream listed repeated disconnect/reconnect stability as unverified; this is
# that check. One compact line per cycle, plus the health/stats deltas.
$ErrorActionPreference = 'Continue'
$log = 'C:\SwitchWOA\wifi-soak.log'
$Profile = 'CMCC-ZouLX-5G'
$Cycles = 10

function Say($m) { "[{0}] {1}" -f (Get-Date -Format 'HH:mm:ss'), $m | Add-Content -Path $log }

function Stats {
    $p = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Enum\ACPI\SWOA0D01\0\Device Parameters' `
        -Name SwitchWifiReport -ErrorAction SilentlyContinue).SwitchWifiReport
    if (-not $p) { return 'erx=? kadd=?' }
    $f = @{}
    foreach ($kv in ($p -split ' ')) { $x = $kv -split '=', 2; if ($x.Count -eq 2) { $f[$x[0]] = $x[1] } }
    "erx=$($f['erx']) kadd=$($f['kadd']) kfail=$($f['kfail']) dn=$($f['dn']) join=$($f['join'])"
}

function Device {
    $d = Get-PnpDevice -InstanceId 'ACPI\SWOA0D01\0' -ErrorAction SilentlyContinue
    "$($d.Status)/$($d.ProblemCode)"
}

Say "=== 0.7.4.0 soak: $Cycles cycles ==="
Say ("start: driver=" + (Get-PnpDeviceProperty -InstanceId 'ACPI\SWOA0D01\0' -KeyName DEVPKEY_Device_DriverVersion).Data + " device=$(Device) $(Stats)")

$pass = 0; $fail = 0
for ($i = 1; $i -le $Cycles; $i++) {
    $t0 = Get-Date
    & netsh wlan disconnect | Out-Null
    Start-Sleep -Seconds 2
    $downOk = (Device) -eq 'OK/'
    & netsh wlan connect name=$Profile | Out-Null
    $up = $false
    for ($t = 1; $t -le 12; $t++) {
        Start-Sleep -Seconds 2
        if ((Get-NetAdapter -Name 'WLAN' -ErrorAction SilentlyContinue).Status -eq 'Up') { $up = $true; break }
    }
    $speed = (Get-NetAdapter -Name 'WLAN' -ErrorAction SilentlyContinue).LinkSpeed
    $secs = [math]::Round(((Get-Date) - $t0).TotalSeconds, 1)
    if ($up -and $downOk) { ++$pass; $verdict = 'OK' } else { ++$fail; $verdict = 'FAIL' }
    Say ("cycle {0,2}: {1,-4} reconnected={2,-5} {3,5}s speed={4} device={5} {6}" -f `
        $i, $verdict, $up, $secs, $speed, (Device), (Stats))
}

Say ("=== result: {0}/{1} cycles reconnected; device={2} ===" -f $pass, $Cycles, (Device))
Say ("final: $(Stats)")
Say "=== end ==="
'soak finished' | Set-Content 'C:\SwitchWOA\wifi-soak.done'
