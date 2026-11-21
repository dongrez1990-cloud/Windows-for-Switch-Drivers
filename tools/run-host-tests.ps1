# Build and run the host (PC) tests for the T210 platform layers.
#
#   powershell -NoProfile -File run-host-tests.ps1 [-Only t210_clock]
#
# This dev box has clang + the Windows SDK headers/libs but no MSVC vcruntime,
# so tests are compiled with /nodefaultlib, linked against Kernel32 only and use
# their own entry point (host_main) plus a local assert(). That keeps the real
# platform .c files compilable and runnable here instead of on the console.
param([string]$Only = '')
$ErrorActionPreference = 'Stop'
$clang = 'C:\Program Files\LLVM\bin\clang-cl.exe'
$kits = 'C:\Program Files (x86)\Windows Kits\10'
$ver = '10.0.26100.0'
$inc = @("$kits\Include\$ver\ucrt", "$kits\Include\$ver\um", "$kits\Include\$ver\shared")
$libArgs = @('/nodefaultlib', '/subsystem:console', '/entry:host_main',
             "$kits\Lib\$ver\um\x64\kernel32.lib")
$plat = 'D:\paopao\erista-gpu\src\mesa-switch-woa\tegra-x1-wddm\platform\t210'
$out = 'D:\paopao\WoA\erista-gpu\tests\host-bin'
New-Item -ItemType Directory -Path $out -Force | Out-Null

$tests = @(
    @{ name = 't210_clock'
       src  = 'D:\paopao\WoA\erista-gpu\tests\test_clock_t210_host.c'
       more = @("$plat\t210_opp.c") }
    @{ name = 'present_ref'
       src  = 'D:\paopao\WoA\erista-gpu\tests\present_ref.c'
       more = @() }
)

$passed = 0
$failed = 0
foreach ($t in $tests) {
    if ($Only -and $t.name -notlike "*$Only*") { continue }
    if (-not (Test-Path $t.src)) { Write-Host ("SKIP missing " + $t.src); continue }
    $exe = Join-Path $out ($t.name + '.exe')
    Remove-Item $exe -Force -ErrorAction SilentlyContinue
    $flags = @('--target=x86_64-pc-windows-msvc', '/std:c11', '/O1', '/nologo', '/GS-',
               '/clang:-Wno-unused-parameter', '/clang:-Wno-unused-function',
               '/clang:-Wno-missing-braces', "$kits\Include") +
             ($inc | ForEach-Object { "/imsvc$_" })
    & $clang @flags $t.src @($t.more) "/Fo:$out\" "/Fe:$exe" /link @libArgs 2>&1 |
        Select-Object -Last 6
    if (-not (Test-Path $exe)) { Write-Host ("BUILD FAIL " + $t.name); $failed++; continue }
    & $exe
    if ($LASTEXITCODE -eq 0) { Write-Host ("PASS " + $t.name); $passed++ }
    else { Write-Host ("FAIL " + $t.name + " (exit " + $LASTEXITCODE + ")"); $failed++ }
}
Write-Host ("host tests: passed={0} failed={1}" -f $passed, $failed)
if ($failed -gt 0) { exit 1 }
