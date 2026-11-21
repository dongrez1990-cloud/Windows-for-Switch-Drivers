# Live (SSH, elevated) rebind: stage a KMD package, point the device class key at it,
# request a PnP reinstall via ConfigFlags, arm the bring-up switches, then reboot.
#   powershell -File rebind-live.ps1 -Version 0.1.0.80 [-Limit 1] [-Mask 0x9E] [-Reboot]
param(
    [string]$Package = 'kmd-package-080',
    [string]$Version = '0.1.0.80',
    [int]$Limit = 1,
    [int]$Mask = 0x9E,
    [int]$ClockFirst = 0,
    [int]$RequireTdStatus = 0,
    [switch]$Reboot
)
$ErrorActionPreference = 'Stop'
$key = 'D:\paopao\ssh\woa_ed25519'
$kh  = 'D:\paopao\ssh\known_hosts'
$pkg = Join-Path 'D:\paopao\WoA\erista-gpu' $Package
if (-not (Test-Path $pkg)) { throw "missing package dir: $pkg" }

foreach ($f in 'tegragpurender.sys', 'tegragpurender.inf', 'tegragpurender.cat') {
    & scp -i $key -o UserKnownHostsFile=$kh -o StrictHostKeyChecking=accept-new -o BatchMode=yes "$pkg\$f" "woa@192.168.1.74:C:/SwitchWOA/gpu-pkg/$f"
    if ($LASTEXITCODE -ne 0) { throw "scp failed for $f" }
}

$rebootFlag = '0'
if ($Reboot) { $rebootFlag = '1' }
$remote = @'
$ErrorActionPreference = 'Continue'
$Version = '__VERSION__'
'--- stage package ---'
$out = & pnputil /add-driver C:\SwitchWOA\gpu-pkg\tegragpurender.inf 2>&1
($out | Out-String) -split "`n" | Where-Object { $_ -match 'oem|成功|failed|error' } | Select-Object -First 4

'--- find oem inf for the version ---'
$target = $null
foreach ($f in Get-ChildItem 'C:\Windows\INF' -Filter 'oem*.inf') {
    $txt = Get-Content $f.FullName -ErrorAction SilentlyContinue
    if (-not ($txt -match 'tegragpurender')) { continue }
    $ver = ($txt | Select-String 'DriverVer=' | Select-Object -First 1).Line
    if ($ver -and $ver -match [regex]::Escape($Version)) { $target = $f.Name; break }
}
'inf=' + $target
if (-not $target) { 'ERROR: no matching oem inf staged'; exit 1 }

'--- point the device class key at it ---'
$cls = 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{F01A9D53-3FF6-48D2-9F97-C8A7004BE10C}\0000'
$old = Get-ItemProperty $cls
'class before: InfPath=' + $old.InfPath + ' DriverVersion=' + $old.DriverVersion
try {
    Set-ItemProperty -Path $cls -Name InfPath       -Value $target  -ErrorAction Stop
    Set-ItemProperty -Path $cls -Name DriverVersion -Value $Version -ErrorAction Stop
    $new = Get-ItemProperty $cls
    'class after : InfPath=' + $new.InfPath + ' DriverVersion=' + $new.DriverVersion
} catch {
    'class key write failed: ' + $_.Exception.Message
}

'--- request reinstall on the device instance ---'
$inst = 'HKLM:\SYSTEM\CurrentControlSet\Enum\ACPI\SWOA0F01\0'
try {
    $cf = [int](Get-ItemProperty $inst).ConfigFlags
    Set-ItemProperty -Path $inst -Name ConfigFlags -Value (($cf -bor 0x20)) -Type DWord -ErrorAction Stop
    'ConfigFlags=0x' + ('{0:X}' -f (Get-ItemProperty $inst).ConfigFlags) + ' (reinstall requested)'
} catch {
    'ConfigFlags write failed: ' + $_.Exception.Message
}

'--- bring-up switches ---'
$k = 'HKLM:\SYSTEM\CurrentControlSet\Services\tegragpurender\Parameters'
New-ItemProperty -Path $k -Name HardwareStartLimit -Value __LIMIT__         -PropertyType DWord -Force | Out-Null
New-ItemProperty -Path $k -Name PowerSkipMask      -Value __MASK__          -PropertyType DWord -Force | Out-Null
New-ItemProperty -Path $k -Name ClockFirst         -Value __CLOCKFIRST__    -PropertyType DWord -Force | Out-Null
New-ItemProperty -Path $k -Name RequireTdStatus    -Value __REQUIRETD__     -PropertyType DWord -Force | Out-Null
$p = Get-ItemProperty $k
'brn: Limit=' + $p.HardwareStartLimit + ' Mask=0x' + ('{0:X}' -f $p.PowerSkipMask) + ' ClockFirst=' + $p.ClockFirst + ' RequireTdStatus=' + $p.RequireTdStatus
'bound now: ' + (Get-PnpDeviceProperty -InstanceId 'ACPI\SWOA0F01\0' -KeyName DEVPKEY_Device_DriverVersion -ErrorAction SilentlyContinue).Data
'@
$remote = $remote.Replace('__VERSION__', $Version).Replace('__LIMIT__', "$Limit").Replace('__MASK__', "$Mask").Replace('__CLOCKFIRST__', "$ClockFirst").Replace('__REQUIRETD__', "$RequireTdStatus")
$b64 = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($remote))
& ssh -i $key -o UserKnownHostsFile=$kh -o StrictHostKeyChecking=accept-new -o BatchMode=yes woa@192.168.1.74 "powershell -NoProfile -EncodedCommand $b64"
"ssh exit=$LASTEXITCODE"

if ($rebootFlag -eq '1') {
    "--- rebooting the Switch (pick the GPU0 test entry at Hekate) ---"
    & ssh -i $key -o UserKnownHostsFile=$kh -o StrictHostKeyChecking=accept-new -o BatchMode=yes woa@192.168.1.74 "shutdown /r /t 3 /f"
}
