# Arm the guarded rollback task and verify what actually landed on disk.
$log = 'C:\SwitchWOA\arm-rollback.log'
function Say($m) { $m | Tee-Object -FilePath $log -Append }

Say "=== hashes on device ==="
Get-ChildItem 'C:\SwitchWOA\drivers\switchwifi-073', 'C:\SwitchWOA\drivers\switchwifi-rollback-079' -File |
    ForEach-Object { Say ("{0,8}  {1}  {2}" -f $_.Length, (Get-FileHash $_.FullName -Algorithm SHA256).Hash.Substring(0, 16), $_.FullName) }

Say "=== inf versions ==="
foreach ($i in 'C:\SwitchWOA\drivers\switchwifi-073\switchwifi.inf', 'C:\SwitchWOA\drivers\switchwifi-rollback-079\switchwifi.inf') {
    Say ("{0} -> {1}" -f $i, (Select-String -Path $i -Pattern 'DriverVer' | Select-Object -First 1).Line)
}

Say "=== arming rollback task (fires in 10 minutes unless wifi-073-ok appears) ==="
Remove-Item 'C:\SwitchWOA\wifi-073-ok' -Force -ErrorAction SilentlyContinue
$when = (Get-Date).AddMinutes(10)
$st = $when.ToString('HH:mm')
$args = @('/create', '/tn', 'WifiRollback073',
          '/tr', 'powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\SwitchWOA\wifi-rollback.ps1',
          '/sc', 'once', '/st', $st, '/ru', 'SYSTEM', '/rl', 'HIGHEST', '/f')
$out = & schtasks.exe @args 2>&1
Say ("schtasks: " + ($out -join ' | '))
$t = Get-ScheduledTask -TaskName 'WifiRollback073' -ErrorAction SilentlyContinue
if ($t) { Say ("armed: state={0} start={1} as={2}" -f $t.State, $t.Triggers[0].StartBoundary, $t.Principal.UserId) }
else { Say 'ERROR: task not found after registration' }
Say ("now on device: " + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))
