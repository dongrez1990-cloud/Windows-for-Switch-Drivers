# Run the remote command in remote.txt with retries, and save the output.
#
#   powershell -NoProfile -File rsh-retry.ps1 -Out <file> [-Needles 'HardwareStatus'] [-MaxTries 12]
#
# The console's link drops roughly half of the SSH banner exchanges (TCP connects,
# then no banner), so every pull is retried until the expected marker appears.
param(
    [string]$Out = 'D:\paopao\.tmp\rsh-out.txt',
    [string]$Needles = 'HardwareStatus',
    [int]$MaxTries = 12,
    [string]$CmdFile = 'D:\paopao\.tmp\remote.txt',
    [string]$Ip = '192.168.1.74',
    [string]$User = 'woa',
    [string]$Key = 'D:\paopao\ssh\woa_ed25519'
)
$ErrorActionPreference = 'Continue'
$sso = @('-i', $Key, '-o', 'BatchMode=yes', '-o', 'StrictHostKeyChecking=no',
         '-o', 'ConnectTimeout=30', '-o', 'LogLevel=ERROR', '-o', 'ServerAliveInterval=5')
$cmd = (Get-Content -Raw -Path $CmdFile).TrimEnd("`r", "`n")
$target = "$User@$Ip"
$want = $Needles -split ',' | ForEach-Object { $_.Trim() }

for ($try = 1; $try -le $MaxTries; $try++) {
    $text = (& ssh @sso -n $target $cmd 2>&1 | Out-String)
    $hit = $true
    foreach ($n in $want) { if ($text -notmatch [regex]::Escape($n)) { $hit = $false } }
    if ($hit) {
        Set-Content -Path $Out -Value $text -Encoding UTF8
        Write-Host ("[{0}] ok on try {1} -> {2}" -f (Get-Date -Format 'HH:mm:ss'), $try, $Out)
        exit 0
    }
    $why = ($text -split "`n" | Where-Object { $_ -match 'timed out|denied|refused|reset|not found' } | Select-Object -First 1)
    Write-Host ("[{0}] try {1} failed{2}" -f (Get-Date -Format 'HH:mm:ss'), $try, $(if ($why) { ': ' + $why.Trim() } else { '' }))
    Start-Sleep -Seconds 20
}
Write-Host 'all tries failed'
exit 1
