# On-device (elevated): install the staged 0.1.0.81 package, point the device class
# key at it, clear the disabled flag, request a reinstall and arm the safe bring-up
# switches (RgWrite=0 -> the GPU_RG_CNTRL register is never written).
param(
    [string]$InfPath = 'C:\SwitchWOA\gpu-pkg\tegragpurender.inf',
    [string]$Version = '0.1.0.81',
    [int]$Limit = 1,
    [int]$Mask = 0x9E,
    [int]$RgWrite = 0
)
$ErrorActionPreference = 'Continue'

'--- 1) stage the package ---'
$out = & pnputil /add-driver $InfPath 2>&1
($out | Out-String) -split "`n" | Where-Object { $_ -match 'oem[0-9]+\.inf|���|failed|error' } | Select-Object -First 4

'--- 2) locate the installed oem inf for this version ---'
$target = $null
foreach ($f in Get-ChildItem 'C:\Windows\INF' -Filter 'oem*.inf') {
    $txt = Get-Content $f.FullName -ErrorAction SilentlyContinue
    if (-not ($txt -match 'tegragpurender')) { continue }
    $ver = ($txt | Select-String 'DriverVer=' | Select-Object -First 1).Line
    if ($ver -and $ver -match [regex]::Escape($Version)) { $target = $f.Name; break }
}
'inf=' + $target
if (-not $target) { 'ERROR: package not staged'; exit 1 }

'--- 3) point the device class key at it ---'
$cls = 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{F01A9D53-3FF6-48D2-9F97-C8A7004BE10C}\0000'
$old = Get-ItemProperty $cls
'class before: InfPath=' + $old.InfPath + ' DriverVersion=' + $old.DriverVersion
Set-ItemProperty -Path $cls -Name InfPath       -Value $target  -ErrorAction SilentlyContinue
Set-ItemProperty -Path $cls -Name DriverVersion -Value $Version -ErrorAction SilentlyContinue
$new = Get-ItemProperty $cls
'class after : InfPath=' + $new.InfPath + ' DriverVersion=' + $new.DriverVersion

'--- 4) clear DISABLED, request reinstall ---'
$inst = 'HKLM:\SYSTEM\CurrentControlSet\Enum\ACPI\SWOA0F01\0'
$cf = 0
try { $cf = [int](Get-ItemProperty $inst).ConfigFlags } catch { $cf = 0 }
$newCf = (($cf -band (-bnot 0x1)) -bor 0x20)
Set-ItemProperty -Path $inst -Name ConfigFlags -Value $newCf -Type DWord -ErrorAction SilentlyContinue
'ConfigFlags=0x' + ('{0:X}' -f (Get-ItemProperty $inst).ConfigFlags) + ' (enabled + reinstall)'

'--- 5) bring-up switches ---'
$k = 'HKLM:\SYSTEM\CurrentControlSet\Services\tegragpurender\Parameters'
New-ItemProperty -Path $k -Name HardwareStartLimit -Value $Limit   -PropertyType DWord -Force | Out-Null
New-ItemProperty -Path $k -Name PowerSkipMask      -Value $Mask    -PropertyType DWord -Force | Out-Null
New-ItemProperty -Path $k -Name ClockFirst         -Value 0        -PropertyType DWord -Force | Out-Null
New-ItemProperty -Path $k -Name RequireTdStatus    -Value 0        -PropertyType DWord -Force | Out-Null
New-ItemProperty -Path $k -Name RgWrite            -Value $RgWrite -PropertyType DWord -Force | Out-Null
$p = Get-ItemProperty $k
'brn: Limit=' + $p.HardwareStartLimit + ' Mask=0x' + ('{0:X}' -f $p.PowerSkipMask) + ' ClockFirst=' + $p.ClockFirst + ' RequireTdStatus=' + $p.RequireTdStatus + ' RgWrite=' + $p.RgWrite

'--- done: reboot and pick the GPU0 test entry ---'
