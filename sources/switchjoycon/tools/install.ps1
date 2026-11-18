# Install / inspect switchjoycon on the Switch over SSH.
#
#   pwsh -File tools\install.ps1 -Install          # copy kit + pnputil /add-driver
#   pwsh -File tools\install.ps1 -Probe            # read device + driver state
#   pwsh -File tools\install.ps1 -Restart          # restart both rail devices
#
param(
    [switch]$Install,
    [switch]$Probe,
    [switch]$Restart,
    [string]$Target = '100.106.117.117',
    [string]$Key = 'D:\paopao\ssh\woa_ed25519',
    [string]$Kit = (Join-Path (Split-Path $PSScriptRoot -Parent) 'out')
)
$ErrorActionPreference = 'Continue'
$ssh = "$env:windir\System32\OpenSSH\ssh.exe"
$scp = "$env:windir\System32\OpenSSH\scp.exe"
$base = @('-o','StrictHostKeyChecking=no','-o','UserKnownHostsFile=NUL','-o','BatchMode=yes',
          '-o','ConnectTimeout=15','-i',$Key,"woa@$Target")

function Invoke-Remote([string]$Script) {
    $b64 = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($Script))
    $out = & $ssh @base "powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand $b64" 2>&1
    ($out | ForEach-Object { $_.ToString() }) -join "`n"
}

if ($Install) {
    foreach ($f in 'switchjoycon.sys', 'switchjoycon.inf', 'switchjoycon.cat') {
        $p = Join-Path $Kit $f
        if (!(Test-Path $p)) { throw "missing $p (run tools\build.ps1 -Sign)" }
        & $scp @base $p "woa@${Target}:C:/SwitchWOA/drivers/switchjoycon/$f"
        if ($LASTEXITCODE -ne 0) { throw "scp failed for $f (is the Switch awake?)" }
        Write-Host "copied $f"
    }
    Invoke-Remote @'
$ErrorActionPreference='Continue'
pnputil /add-driver C:\SwitchWOA\drivers\switchjoycon\switchjoycon.inf /install /force
pnputil /restart-device "ACPI\SWOA0E01\1"
pnputil /restart-device "ACPI\SWOA0E02\2"
Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like 'ACPI\SWOA0E0*' } |
    Select-Object Status,Problem,FriendlyName,InstanceId | Format-Table -AutoSize | Out-String -Width 160
'@
}

if ($Probe) {
    Invoke-Remote @'
$ErrorActionPreference='Continue'
"== devices =="
Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like 'ACPI\SWOA0E0*' } |
    Select-Object Status,Problem,FriendlyName,InstanceId | Format-Table -AutoSize | Out-String -Width 160
"== driver =="
pnputil /enum-drivers | Select-String -Pattern 'switchjoycon' -Context 0,4
foreach ($id in 'ACPI\SWOA0E01\1','ACPI\SWOA0E02\2') {
    "== $id =="
    $k = "HKLM:\SYSTEM\CurrentControlSet\Enum\$id\Device Parameters"
    if (Test-Path $k) {
        Get-ItemProperty $k | Select-Object JcState,JcReports,JcErrors,JcTimeouts,JcMacType,JcAcpiInit |
            Format-List | Out-String
    } else { "  (no Device Parameters key)" }
}
Get-Service switchjoycon -ErrorAction SilentlyContinue | Select-Object Status,Name | Format-Table -AutoSize | Out-String
'@
}

if ($Restart) {
    Invoke-Remote @'
pnputil /restart-device "ACPI\SWOA0E01\1"
pnputil /restart-device "ACPI\SWOA0E02\2"
'@
}
