# Arm the 0.7.4.0 rollout: install, then the reconnect test, with adapter-restart
# backstops and a guarded driver rollback if the new build never brings WLAN up.
function Arm($name, $script, $minutes) {
    $st = (Get-Date).AddMinutes($minutes).ToString('HH:mm')
    $a = @('/create', '/tn', $name,
            '/tr', "powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\SwitchWOA\$script",
            '/sc', 'once', '/st', $st, '/ru', 'SYSTEM', '/rl', 'HIGHEST', '/f')
    $o = & schtasks.exe @a 2>&1
    "{0,-16} start={1}  {2}" -f $name, $st, $(if ($LASTEXITCODE -eq 0) { 'armed' } else { "FAILED: $o" })
}

# Disarm the previous "0.7.3 verified" marker so the rollback task will act.
Remove-Item 'C:\SwitchWOA\wifi-073-ok' -Force -ErrorAction SilentlyContinue
Remove-Item 'C:\SwitchWOA\wifi-lrtest2.done' -Force -ErrorAction SilentlyContinue
Remove-Item 'C:\SwitchWOA\wifi-lrtest2.log' -Force -ErrorAction SilentlyContinue

"=== 0.7.4.0 payload ==="
Get-ChildItem 'C:\SwitchWOA\drivers\switchwifi-074' -File |
    ForEach-Object { "{0,8}  {1}  {2}" -f $_.Length, (Get-FileHash $_.FullName -Algorithm SHA256).Hash.Substring(0,16), $_.Name }
(Select-String -Path 'C:\SwitchWOA\drivers\switchwifi-074\switchwifi.inf' -Pattern 'DriverVer').Line

Arm 'WifiInstall074' 'install-074.ps1' 1
Arm 'WifiLrTest2'    'wifi-lrtest2.ps1' 4
Arm 'WifiRecoverA'   'wifi-recover.ps1' 9
Arm 'WifiRecoverB'   'wifi-recover.ps1' 16
Arm 'WifiRollback074' 'wifi-rollback.ps1' 22

"=== tasks ==="
Get-ScheduledTask | Where-Object { $_.TaskName -match 'Wifi' } |
    Select-Object TaskName, State, @{n='Start';e={$_.Triggers[0].StartBoundary}} | Format-Table -AutoSize | Out-String
"now: " + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss')
