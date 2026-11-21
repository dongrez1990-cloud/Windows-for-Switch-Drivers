# Set the boot GPCPLL point on the console and optionally reboot into it.
#
#   powershell -NoProfile -File set-boot-khz.ps1 -Khz 691200 [-NoReboot]
#
# Only Parameters\BootGpuKhz changes; the working bring-up switches are
# re-asserted so a ladder step can never run with a half-restored switch set.
param(
    [Parameter(Mandatory=$true)][int]$Khz,
    [int]$Limit = 5,
    [int]$Mask = 0x0,
    [int]$RgWrite = 1,
    [int]$ClampNeedsTd = 0,
    [switch]$NoReboot,
    [string]$Ip = '192.168.1.74',
    [string]$User = 'woa',
    [string]$Key = 'D:\paopao\ssh\woa_ed25519'
)
$ErrorActionPreference = 'Stop'
$sso = @('-i', $Key, '-o', 'BatchMode=yes', '-o', 'StrictHostKeyChecking=no', '-o', 'LogLevel=ERROR',
         '-o', 'ConnectTimeout=10')

$remote = @"
`$ErrorActionPreference='Continue'
`$k='HKLM:\SYSTEM\CurrentControlSet\Services\tegragpurender\Parameters'
Set-ItemProperty `$k BootGpuKhz $Khz -Type DWord
Set-ItemProperty `$k HardwareStartLimit $Limit -Type DWord
Set-ItemProperty `$k PowerSkipMask $Mask -Type DWord
Set-ItemProperty `$k RgWrite $RgWrite -Type DWord
Set-ItemProperty `$k ClampNeedsTd $ClampNeedsTd -Type DWord
Set-ItemProperty `$k ClockFirst 0 -Type DWord
Set-ItemProperty `$k RequireTdStatus 0 -Type DWord
`$p=Get-ItemProperty `$k
'BootGpuKhz={0} Limit={1} Mask=0x{2:X} RgWrite={3} ClampNeedsTd={4}' -f `$p.BootGpuKhz,`$p.HardwareStartLimit,`$p.PowerSkipMask,`$p.RgWrite,`$p.ClampNeedsTd
"@

$b64 = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($remote))
$out = & ssh @sso -n ("{0}@{1}" -f $User, $Ip) "powershell -NoProfile -EncodedCommand $b64" 2>&1
$out | ForEach-Object { Write-Host $_ }
if (($out -join '') -notmatch "BootGpuKhz=$Khz") { throw 'switch write not confirmed' }

if ($NoReboot) { Write-Host 'set only (no reboot)'; exit 0 }
& ssh @sso -n ("{0}@{1}" -f $User, $Ip) 'shutdown /r /t 3 /f' 2>&1 | Out-Null
Write-Host ('reboot requested at {0} for {1} kHz' -f (Get-Date -Format 'HH:mm:ss'), $Khz)
