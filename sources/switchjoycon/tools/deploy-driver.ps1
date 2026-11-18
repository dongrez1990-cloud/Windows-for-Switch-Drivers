# deploy-driver.ps1 — push out\switchjoycon.* to the Switch, install, verify.
#   pwsh -File tools\deploy-driver.ps1                 # wait for the box, push, install, report
#   pwsh -File tools\deploy-driver.ps1 -NoInstall      # only push + verify hashes
#   pwsh -File tools\deploy-driver.ps1 -WaitTries 90 -GapSec 20
param(
  [string]$Target   = '100.106.117.117',
  [string]$Key      = 'D:\paopao\ssh\woa_ed25519',
  [string]$Root     = (Split-Path $PSScriptRoot -Parent),
  [int]$WaitTries   = 20,
  [int]$GapSec      = 15,
  [switch]$NoInstall
)
$ErrorActionPreference = 'Continue'
$ProgressPreference = 'SilentlyContinue'

$src = Join-Path $Root 'out'
$remoteDir = 'C:/SwitchWOA/drivers/switchjoycon'
$opt = @('-i', $Key, '-o', 'StrictHostKeyChecking=no', '-o', 'UserKnownHostsFile=NUL',
         '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=10', '-o', 'ServerAliveInterval=10')

function Ssh([string]$cmd) {
  $out = & ssh @opt "woa@$Target" $cmd 2>&1
  return @{ ok = ($LASTEXITCODE -eq 0); text = (($out | ForEach-Object { $_.ToString() }) -join "`n") }
}
function Ps([string]$script) {
  $b64 = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($script))
  return Ssh "powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand $b64"
}

$infText = Get-Content -Raw (Join-Path $Root 'switchjoycon.inf')
$wantVersion = ([regex]::Match($infText, 'DriverVer=\d+/\d+/\d+,([\d\.]+)')).Groups[1].Value
Write-Output "target version: $wantVersion"

# 1. wait for the box
$up = $false
for ($i = 1; $i -le $WaitTries; $i++) {
  $r = Ssh 'hostname'
  if ($r.ok -and $r.text -match 'SWITCH') { Write-Output "machine up (try $i)"; $up = $true; break }
  if ($i -eq $WaitTries) { Write-Output "machine unreachable after $WaitTries tries"; exit 1 }
  Start-Sleep -Seconds $GapSec
}

# 2. push + verify hash of each file
$localHash = @{}
foreach ($f in 'switchjoycon.sys', 'switchjoycon.inf', 'switchjoycon.cat') {
  $localHash[$f] = (Get-FileHash (Join-Path $src $f) -Algorithm SHA256).Hash
}
foreach ($f in 'switchjoycon.sys', 'switchjoycon.inf', 'switchjoycon.cat') {
  $ok = $false
  for ($try = 1; $try -le 5 -and -not $ok; $try++) {
    & scp @opt (Join-Path $src $f) "woa@${Target}:$remoteDir/$f" 2>&1 |
        Where-Object { $_ -notmatch 'Permanently added' } | Out-Null
    $r = Ps @"
`$ProgressPreference='SilentlyContinue'
(Get-FileHash 'C:\SwitchWOA\drivers\switchjoycon\$f' -Algorithm SHA256).Hash
"@
    if ($r.text -match ([regex]::Escape($localHash[$f]))) { $ok = $true }
    else { Start-Sleep -Seconds 5 }
  }
  if (-not $ok) { Write-Output "PUSH FAILED (hash mismatch): $f"; exit 1 }
  Write-Output "pushed ok: $f ($($localHash[$f].Substring(0,16)))"
}

if ($NoInstall) { exit 0 }

# 3. install from a process that does not belong to this SSH session
$tag = ($wantVersion -replace '\.', '')
$r = Ps @"
`$ProgressPreference='SilentlyContinue'
Remove-Item C:\SwitchWOA\pnputil-$tag.log -ErrorAction SilentlyContinue
`$cmd = 'cmd.exe /c C:\Windows\System32\pnputil.exe /add-driver $remoteDir/switchjoycon.inf /install > C:\SwitchWOA\pnputil-$tag.log 2>&1'
`$x = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{ CommandLine = `$cmd }
"install started pid=`$(`$x.ProcessId)"
"@
Write-Output ($r.text -split "`n" | Where-Object { $_ -match 'install started' })

# 4. verify
for ($i = 1; $i -le 10; $i++) {
  Start-Sleep -Seconds 8
  $r = Ps @"
`$ProgressPreference='SilentlyContinue'
`$done = -not (Get-Process pnputil -ErrorAction SilentlyContinue)
foreach (`$id in 'ACPI\SWOA0E01\1','ACPI\SWOA0E02\2') {
  `$v = (Get-PnpDeviceProperty -InstanceId `$id -KeyName DEVPKEY_Device_DriverVersion -ErrorAction SilentlyContinue).Data
  `$d = (Get-PnpDeviceProperty -InstanceId `$id -KeyName DEVPKEY_Device_DriverInfPath -ErrorAction SilentlyContinue).Data
  `$s = (Get-PnpDevice -InstanceId `$id -ErrorAction SilentlyContinue).Status
  "`$id -> `$v (`$d) [`$s] done=`$done"
}
"@
  $lines = $r.text -split "`n" | Where-Object { $_ -match 'SWOA0E0' }
  Write-Output ($lines -join "`n")
  if (($lines -join ' ') -match [regex]::Escape($wantVersion) -and ($lines -join ' ') -match 'done=True') { break }
}

# 5. rail diagnosis
Start-Sleep -Seconds 8
$r = Ps @"
`$ProgressPreference='SilentlyContinue'
if (Test-Path C:\SwitchWOA\pnputil-$tag.log) { "== log =="; (Get-Content C:\SwitchWOA\pnputil-$tag.log -Encoding Default | Select-Object -Last 5) -join "`n" }
"== diag =="
foreach (`$id in 'ACPI\SWOA0E01\1','ACPI\SWOA0E02\2') {
  `$p = Get-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Enum\`$id\Device Parameters" -ErrorAction SilentlyContinue
  "`$id JcVer=`$(`$p.JcVer) state=`$(`$p.JcState) reports=`$(`$p.JcReports) badcrc=`$(`$p.JcBadCrc) badlen=`$(`$p.JcBadLen) rxerr=`$(`$p.JcRxErr) calrej=`$(`$p.JcCalRej) callock=`$(`$p.JcCalLock) centre=`$('{0:X8}' -f [uint32]`$p.JcCentre)"
}
"@
Write-Output $r.text
