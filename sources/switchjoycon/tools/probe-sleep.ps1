# probe-sleep.ps1 — post-mortem for "sleep -> WiFi gone, Settings won't open, Win key dead".
# Run it right after the reboot: the System log still holds the failed resume sequence.
$ProgressPreference = 'SilentlyContinue'
$ErrorActionPreference = 'Continue'

"== boot / uptime =="
$os = Get-CimInstance Win32_OperatingSystem
"  last boot : $($os.LastBootUpTime)"
"  uptime    : $((Get-Date) - $os.LastBootUpTime)"
"  model     : $((Get-CimInstance Win32_ComputerSystem).Model)"

"== sleep states available =="
(powercfg /a 2>&1 | Out-String)

"== last wake source =="
(powercfg /lastwake 2>&1 | Out-String)

"== Kernel-Power timeline (last 6 h) =="
try {
    Get-WinEvent -FilterHashtable @{ LogName = 'System'; StartTime = (Get-Date).AddHours(-6) } -ErrorAction Stop |
        Where-Object { $_.ProviderName -match 'Kernel-Power|Power-Troubleshooter|Kernel-Boot' } |
        Select-Object -First 25 TimeCreated, Id, ProviderName, @{n='Msg';e={ ($_.Message -split "`n")[0] }} |
        Format-Table -AutoSize | Out-String -Width 200
} catch { "  no events: $($_.Exception.Message)" }

"== anything that failed resume (errors/warnings, last 6 h) =="
try {
    Get-WinEvent -FilterHashtable @{ LogName = 'System'; Level = 1,2,3; StartTime = (Get-Date).AddHours(-6) } -ErrorAction Stop |
        Select-Object -First 25 TimeCreated, Id, ProviderName, @{n='Msg';e={ ($_.Message -split "`n")[0] }} |
        Format-Table -AutoSize | Out-String -Width 200
} catch { "  none" }

"== WiFi adapter =="
Get-NetAdapter -ErrorAction SilentlyContinue | Select-Object Name, Status, LinkSpeed, ifIndex |
    Format-Table -AutoSize | Out-String -Width 120
Get-Service switchwifi, switchbt, switchjoycon, switchfan, switchaudio 2>$null |
    Select-Object Status, StartType, Name | Format-Table -AutoSize | Out-String

"== devices with a problem =="
Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
    Where-Object { $_.Status -ne 'OK' } |
    Select-Object Status, Class, FriendlyName, InstanceId | Format-Table -AutoSize | Out-String -Width 180

"== rail driver state right after boot =="
foreach ($id in 'ACPI\SWOA0E01\1', 'ACPI\SWOA0E02\2') {
    $ver = (Get-PnpDeviceProperty -InstanceId $id -KeyName DEVPKEY_Device_DriverVersion -ErrorAction SilentlyContinue).Data
    $p = Get-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Enum\$id\Device Parameters" -ErrorAction SilentlyContinue
    "$id ver=$ver state=$($p.JcState) reports=$($p.JcReports) calrej=$($p.JcCalRej) callock=$($p.JcCalLock) centre=$('{0:X8}' -f [uint32]$p.JcCentre)"
}

"== touch driver (phantom-touch suspect) =="
Get-Service switchtouch -ErrorAction SilentlyContinue | Select-Object Status, Name | Format-Table -AutoSize | Out-String
(Get-PnpDevice -Class HIDClass -PresentOnly -ErrorAction SilentlyContinue |
    Where-Object { $_.FriendlyName -match 'Touch|touch' } |
    Select-Object Status, FriendlyName, InstanceId | Format-Table -AutoSize | Out-String -Width 180)
