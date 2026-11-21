# Package + sign the T210 render miniport for the console.
#
#   powershell -NoProfile -File package-kmd.ps1 -Version 0.1.0.110 [-Src <sys>]
#
# Order matters (HANDOFF 7): replace the sys, bump the INF DriverVer, sign the
# sys FIRST, then Inf2Cat (it hashes the sys into the .cat), then sign the .cat.
# Signing the cat before the sys leaves a stale hash and pnputil rejects it.
param(
    [Parameter(Mandatory=$true)][string]$Version,
    [string]$Src = 'D:\paopao\WoA\erista-gpu\kmd-build\tegragpurender.sys',
    [string]$PkgDir = 'D:\paopao\WoA\erista-gpu\kmd-package'
)
$ErrorActionPreference = 'Stop'
$inf2cat = 'D:\paopao\WoA\erista-gpu\wdk\extracted\c\bin\10.0.28000.0\x86\Inf2Cat.exe'

if (-not (Test-Path $Src)) { throw "missing $Src" }
$sys = Join-Path $PkgDir 'tegragpurender.sys'
$inf = Join-Path $PkgDir 'tegragpurender.inf'
$cat = Join-Path $PkgDir 'tegragpurender.cat'

Copy-Item $Src $sys -Force
$text = Get-Content $inf -Raw
$text = $text -replace 'DriverVer\s*=.*', ("DriverVer=11/21/2026," + $Version)
Set-Content $inf $text -Encoding ascii -NoNewline

$cert = Get-ChildItem Cert:\CurrentUser\My |
    Where-Object { $_.Subject -eq 'CN=SwitchWOA Test Driver Signing' -and $_.HasPrivateKey } |
    Select-Object -First 1
if (-not $cert) { throw 'signing certificate not found' }

$s1 = Set-AuthenticodeSignature -FilePath $sys -Certificate $cert -HashAlgorithm SHA256
if ($s1.Status -ne 'Valid' -and $s1.Status -ne 'UnknownError') { Write-Host ("sys signature: " + $s1.Status) }

Remove-Item $cat -Force -ErrorAction SilentlyContinue
& $inf2cat "/driver:$PkgDir" /os:10_RS5_ARM64 2>&1 | Select-Object -Last 2
if (-not (Test-Path $cat)) { throw 'Inf2Cat produced no catalog' }
$s2 = Set-AuthenticodeSignature -FilePath $cat -Certificate $cert -HashAlgorithm SHA256
if ($s2.Status -ne 'Valid' -and $s2.Status -ne 'UnknownError') { Write-Host ("cat signature: " + $s2.Status) }

"version : " + (Select-String $inf -Pattern 'DriverVer').Line
"sys     : {0:N0} B  sha256 {1}" -f (Get-Item $sys).Length, (Get-FileHash $sys -Algorithm SHA256).Hash
"cat     : {0:N0} B  sha256 {1}" -f (Get-Item $cat).Length, (Get-FileHash $cat -Algorithm SHA256).Hash
