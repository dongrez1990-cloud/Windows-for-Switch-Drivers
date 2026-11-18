$ProgressPreference = 'SilentlyContinue'
$ErrorActionPreference = 'Continue'

"== driver version bound =="
foreach ($id in 'ACPI\SWOA0E01\1', 'ACPI\SWOA0E02\2') {
    $ver = (Get-PnpDeviceProperty -InstanceId $id -KeyName DEVPKEY_Device_DriverVersion -ErrorAction SilentlyContinue).Data
    $inf = (Get-PnpDeviceProperty -InstanceId $id -KeyName DEVPKEY_Device_DriverInfPath -ErrorAction SilentlyContinue).Data
    $st  = (Get-PnpDevice -InstanceId $id -ErrorAction SilentlyContinue).Status
    "$id -> $ver ($inf) [$st]"
}

"== rail diagnostics (v0.3.0 keys) =="
foreach ($id in 'ACPI\SWOA0E01\1', 'ACPI\SWOA0E02\2') {
    $k = "HKLM:\SYSTEM\CurrentControlSet\Enum\$id\Device Parameters"
    "--- $id ---"
    if (-not (Test-Path $k)) { "  (no Device Parameters)"; continue }
    $p = Get-ItemProperty $k
    $names = 'JcVer','JcState','JcReports','JcErrors','JcTimeouts','JcRxBytes','JcFrames','JcFeeds',
             'JcBadCrc','JcBadLen','JcRxErr','JcAxisRej','JcMouseRej','JcPtrOn',
             'JcDetect','JcScratch','JcIrq','JcIir','JcLsr','JcTxPackets',
             'JcLastCmd','JcLastLen','JcLastBtn','JcMacType'
    foreach ($n in $names) {
        $v = $p.$n
        if ($null -ne $v) { "  {0,-12} {1}" -f $n, $v }
    }
    if ($p.JcLastFrame) { "  JcLastFrame  $($p.JcLastFrame)" }
    if ($p.JcRxTail)    { "  JcRxTail     $($p.JcRxTail)" }
}

"== HID children (mouse/keyboard/gamepad) =="
Get-PnpDevice -Class HIDClass -PresentOnly -ErrorAction SilentlyContinue |
    Where-Object { $_.InstanceId -like 'HID\HID_DEVICE_SYSTEM_VHF*' -or $_.FriendlyName -match 'VHF' } |
    Select-Object Status, FriendlyName, InstanceId | Format-Table -AutoSize | Out-String -Width 200
