# Offline (UMS, elevated): install the new GPU bring-up stack on the offline Windows
# volume without any live PnP operation.
#   - switchfan.sys  : fan/backlight + MAX77620 raw-register bridge (VDD_GPU enable)
#   - tegragpurender.sys : render miniport (drives GPIO6 before programming the rail)
# Both .sys carry their own trusted test signature, so replacing the installed file
# is enough; the INF/service identity stays as it is.
param(
    [string]$SystemHive = 'F:\Windows\System32\config\SYSTEM',
    [string]$KmdSys      = 'F:\SwitchWOA\gpu-pkg\tegragpurender.sys',
    [string]$FanSys      = 'F:\SwitchWOA\fan-pkg\switchfan.sys',
    [int]$Limit          = 1,
    [int]$Mask           = 0x80,
    [int]$RgWrite        = 0,
    [int]$RailSweep      = 0,
    [int]$ClampNeedsTd   = 1,
    [int]$ApertureProbe  = 0,
    [int]$RailProbe      = 0,
    [int]$ToggleProbe    = 0
)
$ErrorActionPreference = 'Stop'

$targets = @(
    @{ Sys = $KmdSys; Name = 'tegragpurender.sys' },
    @{ Sys = $FanSys; Name = 'switchfan.sys' }
)
$store = 'F:\Windows\System32\DriverStore\FileRepository'

foreach ($t in $targets) {
    if (-not (Test-Path $t.Sys)) { throw ("missing " + $t.Sys) }
    $newHash = (Get-FileHash $t.Sys -Algorithm SHA256).Hash
    $dest = 'F:\Windows\System32\drivers\' + $t.Name
    if (Test-Path $dest) {
        & takeown.exe /f $dest | Out-Null
        & icacls.exe $dest /grant '*S-1-5-32-544:F' /c | Out-Null
        Copy-Item $t.Sys $dest -Force
        $ok = ((Get-FileHash $dest -Algorithm SHA256).Hash -eq $newHash)
        Write-Host ("  " + $t.Name + " -> " + $dest.Substring(0,20) + "... match=" + $ok)
    } else {
        Write-Host ("  driver file missing on volume: " + $dest)
    }
    $pkgDir = Get-ChildItem $store -Directory -Filter ($t.Name.Replace('.sys','') + '*') -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($pkgDir) {
        $storeFile = Join-Path $pkgDir.FullName $t.Name
        if (Test-Path $storeFile) {
            & takeown.exe /f $storeFile | Out-Null
            & icacls.exe $storeFile /grant '*S-1-5-32-544:F' /c | Out-Null
            Copy-Item $t.Sys $storeFile -Force
            Write-Host ("  store copy updated: " + $pkgDir.Name)
        }
    }
}

& reg.exe load HKLM\OFFSYS $SystemHive | Out-Null
if ($LASTEXITCODE -ne 0) { throw "reg load failed ($LASTEXITCODE) - run elevated" }
try {
    $sel = (Get-ItemProperty 'HKLM:\OFFSYS\Select' -ErrorAction SilentlyContinue).Current
    if (-not $sel) { $sel = 1 }
    $cs = ('ControlSet{0:D3}' -f [int]$sel)
    Write-Host ("control set: " + $cs)
    $k = "HKLM:\OFFSYS\$cs\Services\tegragpurender\Parameters"
    if (-not (Test-Path $k)) {
        New-Item -Path $k -Force | Out-Null
    }
    New-ItemProperty -Path $k -Name HardwareStartLimit -Value $Limit         -PropertyType DWord -Force | Out-Null
    New-ItemProperty -Path $k -Name PowerSkipMask      -Value $Mask          -PropertyType DWord -Force | Out-Null
    New-ItemProperty -Path $k -Name ClockFirst         -Value 0              -PropertyType DWord -Force | Out-Null
    New-ItemProperty -Path $k -Name RequireTdStatus    -Value 0              -PropertyType DWord -Force | Out-Null
    New-ItemProperty -Path $k -Name RgWrite            -Value $RgWrite       -PropertyType DWord -Force | Out-Null
    New-ItemProperty -Path $k -Name RailSweep          -Value $RailSweep     -PropertyType DWord -Force | Out-Null
    New-ItemProperty -Path $k -Name ClampNeedsTd       -Value $ClampNeedsTd  -PropertyType DWord -Force | Out-Null
    New-ItemProperty -Path $k -Name ApertureProbe      -Value $ApertureProbe -PropertyType DWord -Force | Out-Null
    New-ItemProperty -Path $k -Name RailProbe          -Value $RailProbe     -PropertyType DWord -Force | Out-Null
    New-ItemProperty -Path $k -Name ToggleProbe        -Value $ToggleProbe   -PropertyType DWord -Force | Out-Null
    $p = Get-ItemProperty $k
    Write-Host ('brn: Limit=' + $p.HardwareStartLimit + ' Mask=0x' + ('{0:X}' -f $p.PowerSkipMask) +
                ' RgWrite=' + $p.RgWrite + ' ClampNeedsTd=' + $p.ClampNeedsTd + ' ApertureProbe=' + $p.ApertureProbe +
                ' RailProbe=' + $p.RailProbe + ' ToggleProbe=' + $p.ToggleProbe)
} finally {
    & reg.exe unload HKLM\OFFSYS | Out-Null
}
Write-Host 'done: boot "SWOA GPU0 ACPI SWOA0F01 test"'
