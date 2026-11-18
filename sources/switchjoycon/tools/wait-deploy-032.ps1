# wait-deploy-032.ps1 — wait for the Switch to come back, then push + install 0.3.2.
param(
  [string]$Target = '100.106.117.117',
  [string]$Key    = 'D:\paopao\ssh\woa_ed25519',
  [int]$Tries     = 60,
  [int]$GapSec    = 15
)
$ErrorActionPreference = 'Continue'
$src = 'D:\paopao\switchjoycon\out'
$opt = @('-i', $Key, '-o', 'StrictHostKeyChecking=no', '-o', 'UserKnownHostsFile=NUL',
         '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=8')

function Try-Ssh([string]$cmd) {
  $out = & ssh @opt "woa@$Target" $cmd 2>&1
  return @{ ok = ($LASTEXITCODE -eq 0); text = (($out | ForEach-Object { $_.ToString() }) -join "`n") }
}

for ($i = 1; $i -le $Tries; $i++) {
  $r = Try-Ssh 'hostname'
  if ($r.ok -and $r.text -match 'SWITCH') {
    Write-Output "[$([DateTime]::Now.ToString('HH:mm:ss'))] machine is up (try $i)"
    break
  }
  if ($i -eq $Tries) { Write-Output "[$([DateTime]::Now.ToString('HH:mm:ss'))] gave up after $Tries tries"; exit 1 }
  Start-Sleep -Seconds $GapSec
}

foreach ($f in 'switchjoycon.sys', 'switchjoycon.inf', 'switchjoycon.cat') {
  & scp @opt "$src\$f" "woa@${Target}:C:/SwitchWOA/drivers/switchjoycon/$f" 2>&1 |
      Where-Object { $_ -notmatch 'Permanently added' }
}
Write-Output "files pushed"

$install = @'
$ProgressPreference = 'SilentlyContinue'
Remove-Item C:\SwitchWOA\pnputil-032.log -ErrorAction SilentlyContinue
$cmd = 'cmd.exe /c C:\Windows\System32\pnputil.exe /add-driver C:\SwitchWOA\drivers\switchjoycon\switchjoycon.inf /install > C:\SwitchWOA\pnputil-032.log 2>&1'
$r = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{ CommandLine = $cmd }
"install started pid=$($r.ProcessId)"
'@
$b64 = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($install))
$r = Try-Ssh "powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand $b64"
$r.text -split "`n" | Where-Object { $_ -match 'install started' }

Start-Sleep -Seconds 16

$report = @'
$ProgressPreference = 'SilentlyContinue'
if (Test-Path C:\SwitchWOA\pnputil-032.log) { "== log =="; (Get-Content C:\SwitchWOA\pnputil-032.log -Encoding Default | Select-Object -Last 5) -join "`n" }
"== versions =="
foreach ($id in 'ACPI\SWOA0E01\1','ACPI\SWOA0E02\2') {
  $v = (Get-PnpDeviceProperty -InstanceId $id -KeyName DEVPKEY_Device_DriverVersion -ErrorAction SilentlyContinue).Data
  $d = (Get-PnpDeviceProperty -InstanceId $id -KeyName DEVPKEY_Device_DriverInfPath -ErrorAction SilentlyContinue).Data
  $s = (Get-PnpDevice -InstanceId $id -ErrorAction SilentlyContinue).Status
  "$id -> $v ($d) [$s]"
}
Start-Sleep -Seconds 8
"== diag =="
foreach ($id in 'ACPI\SWOA0E01\1','ACPI\SWOA0E02\2') {
  $p = Get-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Enum\$id\Device Parameters"
  "$id JcVer=$($p.JcVer) state=$($p.JcState) reports=$($p.JcReports) badcrc=$($p.JcBadCrc) badlen=$($p.JcBadLen) rxerr=$($p.JcRxErr) calrej=$($p.JcCalRej) callock=$($p.JcCalLock) centre=$('{0:X8}' -f [uint32]$p.JcCentre) lastbtn=$($p.JcLastBtn)"
}
'@
$b64 = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($report))
$r = Try-Ssh "powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand $b64"
$r.text
