$ProgressPreference = 'SilentlyContinue'
$ErrorActionPreference = 'Continue'

"== date =="
Get-Date -Format o
"== firmware marker =="
try { (Get-CimInstance Win32_ComputerSystem).Model } catch {}
"== rail devices =="
Get-PnpDevice -ErrorAction SilentlyContinue |
    Where-Object { $_.InstanceId -like 'ACPI\SWOA0E0*' -or $_.InstanceId -like '*SWOA0E*' } |
    Select-Object Status, Class, Problem, ProblemDescription, FriendlyName, InstanceId |
    Format-List | Out-String -Width 200

"== enum keys (HWID / driver / problem) =="
foreach ($id in 'ACPI\SWOA0E01\1', 'ACPI\SWOA0E02\2') {
    $k = "HKLM:\SYSTEM\CurrentControlSet\Enum\$id"
    if (Test-Path $k) {
        $p = Get-ItemProperty $k
        "--- $id ---"
        "  DeviceDesc : $($p.DeviceDesc)"
        "  Service    : $($p.Service)"
        "  Driver     : $($p.Driver)"
        "  ConfigFlags: $($p.ConfigFlags)"
        "  HWIDs      : " + (($p.HardwareID) -join ' | ')
        $r = "$k\Device Parameters"
        if (Test-Path $r) {
            $v = Get-ItemProperty $r
            Get-ItemProperty $r | Select-Object JcState, JcReports, JcErrors, JcTimeouts, JcMacType, JcAcpiInit |
                Format-List | Out-String
        } else { "  (no Device Parameters)" }
    } else { "--- $id : NOT PRESENT ---" }
}

"== driver store / service =="
pnputil /enum-drivers 2>&1 | Select-String -Pattern 'switchjoycon' -Context 0,4 | Out-String
Get-Service switchjoycon -ErrorAction SilentlyContinue | Select-Object Status, StartType, Name | Format-Table -AutoSize | Out-String

"== any joy-con HID gamepad? =="
Get-PnpDevice -Class HIDClass -PresentOnly -ErrorAction SilentlyContinue |
    Select-Object Status, FriendlyName, InstanceId | Format-Table -AutoSize | Out-String -Width 160

"== kernel events for the service (last 10) =="
try {
  Get-WinEvent -FilterHashtable @{LogName='System'; ProviderName='Service Control Manager'; StartTime=(Get-Date).AddMinutes(-15)} -ErrorAction Stop |
    Where-Object { $_.Message -match 'switchjoycon' } |
    Select-Object -First 6 TimeCreated, Id, Message | Format-List | Out-String
} catch { "no events: $($_.Exception.Message)" }
