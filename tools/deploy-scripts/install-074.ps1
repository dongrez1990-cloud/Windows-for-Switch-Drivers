# C:\SwitchWOA\install-074.ps1 - install SwitchWifi 0.7.4.0 from a SYSTEM task.
$ErrorActionPreference = 'Continue'
$log = 'C:\SwitchWOA\wifi-install-074.log'
function Say($m) { "[{0}] {1}" -f (Get-Date -Format 'HH:mm:ss'), $m | Add-Content -Path $log }

Say "=== install 0.7.4.0 start ==="
$before = (Get-PnpDeviceProperty -InstanceId 'ACPI\SWOA0D01\0' -KeyName DEVPKEY_Device_DriverVersion).Data
Say ("before: driver={0} status={1}" -f $before, (Get-PnpDevice -InstanceId 'ACPI\SWOA0D01\0').Status)

$out = & pnputil.exe /add-driver 'C:\SwitchWOA\drivers\switchwifi-074\switchwifi.inf' /install 2>&1
Say ("pnputil exit={0}" -f $LASTEXITCODE)
Say ("pnputil output: " + ($out -join ' | '))

Start-Sleep -Seconds 4
$after = (Get-PnpDeviceProperty -InstanceId 'ACPI\SWOA0D01\0' -KeyName DEVPKEY_Device_DriverVersion).Data
Say ("after : driver={0} status={1}" -f $after, (Get-PnpDevice -InstanceId 'ACPI\SWOA0D01\0').Status)

for ($i = 1; $i -le 20; $i++) {
    Start-Sleep -Seconds 3
    $a = Get-NetAdapter -Name 'WLAN' -ErrorAction SilentlyContinue
    if ($a -and $a.Status -eq 'Up') { Say ("link up after ~{0}s: speed={1}" -f ($i*3), $a.LinkSpeed); break }
    if ($i -eq 20) { Say ("link NOT up after 60s: status={0}" -f $(if ($a) { $a.Status } else { 'missing' })) }
}
Say "=== install 0.7.4.0 done ==="
'install-074 finished' | Set-Content 'C:\SwitchWOA\wifi-install-074.done'
