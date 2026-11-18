# Arm the leave-policy test plus two adapter-restart backstops.
# The test itself drops the SSH transport, so it runs from a SYSTEM task.
function Arm($name, $script, $minutes) {
    $st = (Get-Date).AddMinutes($minutes).ToString('HH:mm')
    $a = @('/create', '/tn', $name,
            '/tr', "powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\SwitchWOA\$script",
            '/sc', 'once', '/st', $st, '/ru', 'SYSTEM', '/rl', 'HIGHEST', '/f')
    $o = & schtasks.exe @a 2>&1
    "{0,-16} start={1}  {2}" -f $name, $st, $(if ($LASTEXITCODE -eq 0) { 'armed' } else { "FAILED: $o" })
}

Remove-Item 'C:\SwitchWOA\wifi-lrtest.done' -Force -ErrorAction SilentlyContinue
Remove-Item 'C:\SwitchWOA\wifi-lrtest.log' -Force -ErrorAction SilentlyContinue

Arm 'WifiLrTest'    'wifi-lrtest.ps1'  1
Arm 'WifiRecoverA'  'wifi-recover.ps1' 6
Arm 'WifiRecoverB'  'wifi-recover.ps1' 13
"--- tasks ---"
Get-ScheduledTask | Where-Object { $_.TaskName -match 'Wifi' } |
    Select-Object TaskName, State, @{n='Start';e={$_.Triggers[0].StartBoundary}} | Format-Table -AutoSize | Out-String
"now: " + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss')
