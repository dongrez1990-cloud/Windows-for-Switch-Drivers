# Wait for one boot to finish the GPU bring-up and pull that boot's records.
#
#   powershell -NoProfile -File pull-run.ps1 -ExpectKhz 691200 [-TimeoutMinutes 14]
#
# Freshness gate (v0.1.0.110+): the driver records the point it actually ran in
# ClkNote3, so "ClkNote3 == ExpectKhz and uptime is inside the boot window" only
# matches a boot that consumed this ladder step. The trace mirror is rewritten
# in place with FILE_OVERWRITE_IF, so it is re-read until its size settles.
param(
    [string]$Out = 'D:\paopao\.tmp\pull-run.txt',
    [int]$ExpectKhz = 0,
    [int]$TimeoutMinutes = 14,
    [string]$Ip = '192.168.1.74',
    [string]$User = 'woa',
    [string]$Key = 'D:\paopao\ssh\woa_ed25519'
)
$ErrorActionPreference = 'Continue'
$sso = @('-i', $Key, '-o', 'BatchMode=yes', '-o', 'StrictHostKeyChecking=no',
         '-o', 'ConnectTimeout=8', '-o', 'LogLevel=ERROR', '-o', 'ServerAliveInterval=5')

function Note([string]$s) {
    $line = ('[{0}] {1}' -f (Get-Date -Format 'HH:mm:ss'), $s)
    Add-Content -Path $Out -Value $line
    Write-Host $line
}

$stateCmd = '$p=(Get-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Services\tegragpurender\Parameters");' +
            '"{0} {1} {2}" -f [int64]((Get-Date)-(Get-CimInstance Win32_OperatingSystem).LastBootUpTime).TotalSeconds,[int64]$p.ClkNote3,[int64]$p.ClkNote7'
$paramCmd = 'reg query "HKLM\SYSTEM\CurrentControlSet\Services\tegragpurender\Parameters"'
$sizeCmd  = '(Get-Item C:\SwitchWOA\gpu-run.txt).Length'

Set-Content -Path $Out -Value ('=== pull {0} expect={1} ===' -f (Get-Date -Format 'HH:mm:ss'), $ExpectKhz)
$deadline = (Get-Date).AddMinutes($TimeoutMinutes)
while ((Get-Date) -lt $deadline) {
    $raw = (& ssh @sso -n "$User@$Ip" $stateCmd 2>&1 | Out-String).Trim()
    $parts = ($raw -split "`n")[0].Trim() -split '\s+'
    $uptime = 0; $note3 = 0; $note7 = 0
    if ($parts.Count -lt 3 -or
        -not [int64]::TryParse($parts[0], [ref]$uptime) -or
        -not [int64]::TryParse($parts[1], [ref]$note3) -or
        -not [int64]::TryParse($parts[2], [ref]$note7)) {
        Start-Sleep -Seconds 20
        continue
    }
    Write-Host ("[{0}] up {1}s  ClkNote3={2}  ClkNote7={3}" -f (Get-Date -Format 'HH:mm:ss'), $uptime, $note3, $note7)
    if ($uptime -gt 900) { Note ("machine is still on the previous boot (up {0}s); ClkNote3={1}" -f $uptime, $note3); break }
    if ($uptime -lt 45) { Start-Sleep -Seconds 10; continue }
    if ($ExpectKhz -gt 0 -and $note3 -ne $ExpectKhz) {
        Start-Sleep -Seconds 20
        continue
    }
    if ($note7 -ne $ExpectKhz) {
        Note ("clock stage not finished yet (target {0}, measured {1})" -f $note3, $note7)
        Start-Sleep -Seconds 20
        continue
    }
    Note ("bring-up finished: up {0}s, ClkNote3={1}, ClkNote7={2} -> pulling" -f $uptime, $note3, $note7)
    # Trace mirror: wait for the size to stop changing before reading it.
    $prev = -1
    for ($i = 0; $i -lt 8; $i++) {
        $s = (& ssh @sso -n "$User@$Ip" $sizeCmd 2>&1 | Out-String).Trim()
        if ($s -match '^\d+$' -and [int64]$s -eq $prev) { break }
        if ($s -match '^\d+$') { $prev = [int64]$s }
        Start-Sleep -Seconds 3
    }
    Add-Content -Path $Out -Value ('--- trace size ' + $prev + ' ---')
    Add-Content -Path $Out -Value '--- parameters ---'
    & ssh @sso -n "$User@$Ip" $paramCmd 2>&1 | Add-Content -Path $Out
    Add-Content -Path $Out -Value '--- gpu-run.txt ---'
    & ssh @sso -n "$User@$Ip" 'type C:\SwitchWOA\gpu-run.txt' 2>&1 | Add-Content -Path $Out
    Note 'pull complete'
    exit 0
}
Note 'timed out'
exit 1
