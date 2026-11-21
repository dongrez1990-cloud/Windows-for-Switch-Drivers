# Offline inspection (read-only, needs Administrator + SD card mounted): dump the
# tegragpurender bring-up records, the bugcheck/minidump evidence and the recent
# WHEA/BugCheck/Kernel-Power events to F:\SwitchWOA\gpu-report.txt.
param(
    [string]$SystemHive = 'F:\Windows\System32\config\SYSTEM',
    [string]$EventLog   = 'F:\Windows\System32\winevt\Logs\System.evtx',
    [string]$OutFile    = 'F:\SwitchWOA\gpu-report.txt'
)
$ErrorActionPreference = 'Continue'
$lines = New-Object System.Collections.Generic.List[string]
function Add([string]$s) { $lines.Add($s); Write-Host $s }

Add ("=== offline GPU report " + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss') + " ===")

& takeown.exe /f $SystemHive | Out-Null
& icacls.exe $SystemHive /grant '*S-1-5-32-544:F' /c | Out-Null
& reg.exe load HKLM\OFFSYS $SystemHive | Out-Null
if ($LASTEXITCODE -ne 0) { Add "reg load failed ($LASTEXITCODE) - run elevated" }
else {
    try {
        $sel = (Get-ItemProperty 'HKLM:\OFFSYS\Select' -ErrorAction SilentlyContinue).Current
        if (-not $sel) { $sel = 1 }
        $cs = ('ControlSet{0:D3}' -f [int]$sel)
        Add ("control set: " + $cs)
        $k = 'HKLM:\OFFSYS\' + $cs + '\Services\tegragpurender\Parameters'
        if (Test-Path $k) {
            $p = Get-ItemProperty $k
            $groups = [ordered]@{
                'core' = 'Step','Bc','BcDetail','ResourceStatus','HardwareStatus','HardwareStage','PowerResult','ClockResult','FrontendResult','ExecutionResult','HardwareStartLimit','PowerSkipMask','ClockFirst','RequireTdStatus','RgWrite','RgWriteMode','RgWrote'
                'gpu clamp' = 'BootGpuRg','GpuRgBeforeClear','GpuRgCleared','RgWrote','BootClamp','ClampAfterPowerGate','BootHidrev','BootRstX','RstAsserted','RstX','ClampSt','ClkX'
                'pmc state' = 'PmcCtrl','PmcSecDisable','PmcSecDisable2','PmcPwrDet','PmcPgMask','PmcPgMask1','PmcTdPgTimer','PmcPgMaskAndor','PmcPwrGateTimerOff','PmcPwrGoodTimer'
                'pmc probe' = 'PwrGateBefore','TglBefore','PmcCtrl','PmcSecDisable','PmcPwrGateTimerOff','PmcPwrGoodTimer','VicWasOn','VicToggle1Ack','VicToggle1Status','VicToggle2Ack','VicToggle2Status','TdClampBefore','TdTglAck','TdStatus1','TdStatus1b','TdClampAfterClear','TdTglAck2','TdStatus2','TdStatus2b','PwrGateAfter','TdStatusFinal'
                'car x bank' = 'ClkXBeforeSet','ClkXAfterGate','ClkXAfterSet','Out5Read','RstX','ClkX','ClkXFinal','RstAsserted','BootRstX'
                'aperture' = 'Aperture0','Aperture1','AperturePllCfg','AperturePllOut'
                'clock' = 'BootGpuMeasuredKhz','BootGpuRailUv'
            }
            foreach ($g in $groups.Keys) {
                Add ("--- " + $g + " ---")
                foreach ($n in $groups[$g]) {
                    if ($p.PSObject.Properties.Name -contains $n) {
                        $v = $p.$n
                        if ($v -is [int] -or $v -is [long]) { Add (('{0,-22} {1,12} (0x{1:X})' -f $n, [int64]$v)) }
                        else { Add (('{0,-22} {1}' -f $n, [string]$v)) }
                    }
                }
            }
        } else { Add 'Parameters key missing' }
        Add '--- device / class ---'
        $inst = 'HKLM:\OFFSYS\' + $cs + '\Enum\ACPI\SWOA0F01\0'
        if (Test-Path $inst) {
            $i = Get-ItemProperty $inst
            Add ('ConfigFlags=0x' + ('{0:X}' -f [int]$i.ConfigFlags) + '  Driver=' + $i.Driver + '  Service=' + $i.Service)
        } else { Add 'device instance missing' }
        $cls = 'HKLM:\OFFSYS\' + $cs + '\Control\Class\{F01A9D53-3FF6-48D2-9F97-C8A7004BE10C}\0000'
        if (Test-Path $cls) {
            $c = Get-ItemProperty $cls
            Add ('class: InfPath=' + $c.InfPath + ' DriverVersion=' + $c.DriverVersion)
        }
    } finally { & reg.exe unload HKLM\OFFSYS | Out-Null }
}

Add ''
Add '--- minidumps ---'
$md = 'F:\Windows\Minidump'
if (Test-Path $md) {
    Get-ChildItem $md -Filter '*.dmp' -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending |
        Select-Object -First 5 | ForEach-Object { Add ('{0}  {1:N0} B  {2}' -f $_.LastWriteTime.ToString('MM-dd HH:mm:ss'), $_.Length, $_.Name) }
} else { Add 'no Minidump directory' }
$mem = 'F:\Windows\MEMORY.DMP'
if (Test-Path $mem) { Add ('MEMORY.DMP ' + (Get-Item $mem).LastWriteTime.ToString('MM-dd HH:mm:ss') + ' ' + (Get-Item $mem).Length + ' B') }

Add ''
Add '--- recent WHEA / BugCheck / Kernel-Power / DPCS events ---'
if (Test-Path $EventLog) {
    try {
        $ev = Get-WinEvent -Path $EventLog -MaxEvents 4000 -ErrorAction Stop
        $ev | Where-Object { $_.ProviderName -match 'WHEA|BugCheck|Kernel-Power|Kernel-PnP' } |
            Select-Object -First 14 |
            ForEach-Object { Add ('{0}  {1,-24} id={2}  {3}' -f $_.TimeCreated.ToString('MM-dd HH:mm:ss'), $_.ProviderName, $_.Id, (($_.Message -split "`r?`n")[0])) }
    } catch { Add ("event log read failed: " + $_.Exception.Message) }
} else { Add 'no System.evtx' }

$lines -join "`r`n" | Set-Content -Path $OutFile -Encoding UTF8
Write-Host ('report written: ' + $OutFile)
