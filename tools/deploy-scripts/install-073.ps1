# C:\SwitchWOA\install-073.ps1
# Runs as SYSTEM from a one-shot scheduled task: the install restarts the WLAN
# adapter, which is also the SSH transport, so it must not depend on our session.
$ErrorActionPreference = 'Continue'
$log = 'C:\SwitchWOA\wifi-install-073.log'
function Say($m) { "[{0}] {1}" -f (Get-Date -Format 'HH:mm:ss'), $m | Add-Content -Path $log }

Say "=== install 0.7.3.0 start ==="
$before = (Get-PnpDeviceProperty -InstanceId 'ACPI\SWOA0D01\0' -KeyName DEVPKEY_Device_DriverVersion).Data
Say ("before: driver={0} status={1}" -f $before, (Get-PnpDevice -InstanceId 'ACPI\SWOA0D01\0').Status)

$out = & pnputil.exe /add-driver 'C:\SwitchWOA\drivers\switchwifi-073\switchwifi.inf' /install 2>&1
Say ("pnputil exit={0}" -f $LASTEXITCODE)
Say ("pnputil output: " + ($out -join ' | '))

Start-Sleep -Seconds 4
$after = (Get-PnpDeviceProperty -InstanceId 'ACPI\SWOA0D01\0' -KeyName DEVPKEY_Device_DriverVersion).Data
Say ("after : driver={0} status={1}" -f $after, (Get-PnpDevice -InstanceId 'ACPI\SWOA0D01\0').Status)

# Give the station time to associate before reporting the link.
for ($i = 1; $i -le 20; $i++) {
    Start-Sleep -Seconds 3
    $a = Get-NetAdapter -Name 'WLAN' -ErrorAction SilentlyContinue
    $ssid = (netsh wlan show interfaces | Select-String -Pattern '^\s*SSID') -join ''
    if ($a -and $a.Status -eq 'Up') {
        Say ("link up after ~{0}s: status={1} speed={2}" -f ($i * 3), $a.Status, $a.LinkSpeed)
        break
    }
    if ($i -eq 20) { Say ("link NOT up after 60s: status={0}" -f $(if ($a) { $a.Status } else { 'missing' })) }
}
Say "=== install 0.7.3.0 done ==="
'install-073 finished' | Set-Content 'C:\SwitchWOA\wifi-install-073.done'
