# Build switchjoycon.sys (ARM64 KMDF) with LLVM + the installed WDK.
#
#   pwsh -File tools\build.ps1                 # unsigned (fast iteration)
#   pwsh -File tools\build.ps1 -Sign           # test-signed + .cat for install
#
param(
    [switch]$Sign,
    [string]$Root = (Split-Path $PSScriptRoot -Parent),
    [string]$Builder = 'D:\paopao\WoA\rebuild\src\Drivers\tools\Build-Driver.ps1'
)
$ErrorActionPreference = 'Stop'
$src = Join-Path $Root 'src\switchjoycon.c'
$inf = Join-Path $Root 'switchjoycon.inf'
$out = Join-Path $Root 'out'
$args = @{
    Name    = 'switchjoycon'
    Sources = $src
    OutDir  = $out
    Libs    = @('vhfkm.lib', 'libcntpr.lib')
}
if ($Sign) { $args.Inf = $inf } else { $args.NoSign = $true }
& $Builder @args
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
Get-ChildItem $out | Select-Object Name, Length | Format-Table -AutoSize
