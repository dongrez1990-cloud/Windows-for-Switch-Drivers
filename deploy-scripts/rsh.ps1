# Run a remote command on the WoA console over SSH.
# Reads the remote command text from a local file so no quoting ever crosses a shell boundary.
#   powershell -NoProfile -File rsh.ps1 [-File D:\paopao\.tmp\remote.txt] [-Timeout 60]
param(
    [string]$CmdFile = 'D:\paopao\.tmp\remote.txt',
    [string]$Ip = '192.168.1.74',
    [string]$User = 'woa',
    [string]$Key = 'D:\paopao\ssh\woa_ed25519'
)
$ErrorActionPreference = 'Continue'
$cmd = (Get-Content -Raw -Path $CmdFile)
if ([string]::IsNullOrWhiteSpace($cmd)) { Write-Host 'empty remote command'; exit 2 }
$cmd = $cmd.TrimEnd("`r", "`n")
$sso = @('-i', $Key, '-o', 'BatchMode=yes', '-o', 'StrictHostKeyChecking=no',
         '-o', 'ConnectTimeout=10', '-o', 'LogLevel=ERROR',
         '-o', 'ServerAliveInterval=5', '-o', 'ServerAliveCountMax=3')
$target = "$User@$Ip"
Write-Host ("--- remote: " + $cmd)
& ssh @sso -n $target $cmd
$rc = $LASTEXITCODE
Write-Host ("--- exit " + $rc)
exit $rc
