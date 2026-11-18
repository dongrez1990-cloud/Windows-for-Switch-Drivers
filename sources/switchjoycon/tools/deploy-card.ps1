# Put the Joy-Con firmware variant on the SD card (card in the PC reader).
#
#   pwsh -File tools\deploy-card.ps1                 # auto-detect the ESP
#   pwsh -File tools\deploy-card.ps1 -Drive E: -Name 44-joycon
#
# Layout written on the FAT32 ESP:
#   switchroot\joycon\bl33.bin   (the v44 build)
#   switchroot\joycon\bl31.bin   (copied from switchroot\winx\bl31.bin if absent)
#   bootloader\ini\44-joycon.ini (hekate menu entry)
param(
    [string]$Drive,
    [string]$Name = '44-joycon',
    [string]$Source = 'D:\paopao\WoA\rebuild\out\bl33-swoa-v44.bin'
)
$ErrorActionPreference = 'Stop'
if (!(Test-Path $Source)) { throw "firmware not built: $Source" }

if (!$Drive) {
    $candidates = Get-PSDrive -PSProvider FileSystem | Where-Object {
        Test-Path (Join-Path $_.Root 'switchroot') -ErrorAction SilentlyContinue
    }
    if (!$candidates) { throw 'no card with \switchroot found; pass -Drive E:' }
    $Drive = $candidates[0].Root
}
$esp = $Drive.TrimEnd('\')

$dir = Join-Path $esp 'switchroot\joycon'
New-Item -ItemType Directory -Force $dir | Out-Null
Copy-Item $Source (Join-Path $dir 'bl33.bin') -Force
Write-Host ("bl33 -> {0}  {1} bytes" -f (Join-Path $dir 'bl33.bin'), (Get-Item $Source).Length)

$bl31 = Join-Path $dir 'bl31.bin'
if (!(Test-Path $bl31)) {
    $from = Join-Path $esp 'switchroot\winx\bl31.bin'
    if (!(Test-Path $from)) { throw "no bl31.bin: put one at $from first" }
    Copy-Item $from $bl31 -Force
    Write-Host ("bl31 copied from {0}" -f $from)
}

$ini = Join-Path $esp 'bootloader\ini'
New-Item -ItemType Directory -Force $ini | Out-Null
$iniFile = Join-Path $ini ("{0}.ini" -f $Name)
@"
[Windows 11 + JoyCon v44]
l4t=1
boot_prefixes=switchroot/joycon/
id=JOYCON44
"@ -replace "`n", "`r`n" | Set-Content -Path $iniFile -Encoding ascii
Write-Host ("entry  -> {0}" -f $iniFile)

Get-ChildItem $dir | Select-Object Name, Length | Format-Table -AutoSize
Write-Host 'Reminder: safe-eject the card before pulling it out (one eject removes both volumes).'
