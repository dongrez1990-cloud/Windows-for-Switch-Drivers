<#
.SYNOPSIS
  Compile and run the SwitchWifi host-side unit tests (drivers\SwitchWifi\tests).

.DESCRIPTION
  These tests exercise the portable `core\` layer (msgbuf rings, msgbuf control
  and data paths, EROM/NVRAM parsing, escan, PHY rates, station frames,
  security, filters, RX metadata, PCI INTx) with assertions only - no WDK, no
  device, no kernel.

  The build host has the WDK and LLVM but no MSVC CRT, so a normal hosted
  toolchain is unavailable. Each test is therefore compiled freestanding
  against tools\HostTests\include plus tools\HostTests\shim.c, which supplies
  the small slice of the C library the tests use (memory functions, puts, a
  512-byte printf, malloc/free) over kernel32 imports. That keeps the suite
  runnable with nothing but the LLVM install already required by the driver
  build.

.EXAMPLE
  .\Test-SwitchWifi.ps1
  .\Test-SwitchWifi.ps1 -Filter 'ring|frame'
#>
param(
  # Regex over the test file's base name; '.' runs everything.
  [string]$Filter = '.',
  [string]$OutDir,
  [switch]$NoRun
)

$ErrorActionPreference = 'Stop'
$Repo = Split-Path $PSScriptRoot -Parent
$Drv = Join-Path $Repo 'drivers\SwitchWifi'
$HostDir = Join-Path $PSScriptRoot 'HostTests'
$Llvm = 'C:\Program Files\LLVM\bin'
$Kits = "${env:ProgramFiles(x86)}\Windows Kits\10"
$UmLib = "$Kits\Lib\10.0.26100.0\um\x64"
if (-not $OutDir) { $OutDir = Join-Path $Repo 'build\HostTests' }
New-Item -ItemType Directory -Force $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path

foreach ($p in "$Llvm\clang.exe", (Join-Path $HostDir 'shim.c'), (Join-Path $HostDir 'include\assert.h')) {
  if (-not (Test-Path $p)) { throw "Missing: $p" }
}
if (-not (Test-Path "$UmLib\kernel32.lib")) { throw "Missing: $UmLib\kernel32.lib" }

$ResourceDir = (& "$Llvm\clang.exe" -print-resource-dir).Trim()
if (-not (Test-Path "$ResourceDir\include\stdint.h")) { throw "Missing clang builtin headers under $ResourceDir" }

# Freestanding, no CRT, no default libraries: shim.c provides the entry point
# and chkstk.c provides the x64 stack probe the compiler calls for big frames.
$common = @(
  '--target=x86_64-pc-windows-msvc', '-fuse-ld=lld', '-O1', '-g0',
  '-ffreestanding', '-nostdlibinc', '-nostdlib', '-fno-stack-protector',
  '-Wno-unused-command-line-argument',
  "-isystem", "$ResourceDir\include",
  "-I", (Join-Path $HostDir 'include')
)
$link = @(
  "-Wl,/entry:swifi_host_entry", '-Wl,/subsystem:console', '-Wl,/nodefaultlib',
  "-Wl,/libpath:$UmLib", '-lkernel32'
)

$CoreSources = @(Get-ChildItem (Join-Path $Drv 'core') -Filter *.c | Sort-Object Name | ForEach-Object { $_.FullName })
$Shim = Join-Path $HostDir 'shim.c'
$Chkstk = Join-Path $HostDir 'chkstk.c'
$Tests = @(Get-ChildItem (Join-Path $Drv 'tests') -Filter 'test_*.c' | Sort-Object Name |
    Where-Object { $_.BaseName -match $Filter })
if (-not $Tests.Count) { throw "No tests matched -Filter '$Filter'" }

# Two tests compile *production* Windows code: the join/receive/indication bodies
# are sliced out of windows\*.c into a .inc that the test #includes, with WDF,
# Ndis and the firmware substituted by stubs declared in the test itself. This
# mirrors the upstream extractor in drivers\SwitchWifi\tools\check-core.py. These
# are the regressions that guard the disconnect-timer readback and the RX path
# across a reconnect - the two places where "WiFi never comes back" lives.
function Get-FunctionSlice([string]$Source, [string]$Signature) {
  $start = $Source.IndexOf($Signature)
  if ($start -lt 0) { throw "function not found in source: $Signature" }
  $end = $Source.IndexOf('{', $start)
  if ($end -lt 0) { throw "no body for: $Signature" }
  $depth = 0
  for (; $end -lt $Source.Length; ++$end) {
    if ($Source[$end] -eq '{') { ++$depth }
    elseif ($Source[$end] -eq '}') { --$depth; if ($depth -eq 0) { ++$end; break } }
  }
  if ($depth -ne 0) { throw "unterminated function: $Signature" }
  return $Source.Substring($start, $end - $start)
}

$Slices = @{
  'test_join_timeout' = @{
    SourceFile = (Join-Path $Drv 'windows\join.c')
    IncName    = 'join-timeout-under-test.inc'
    Signatures = @('static NTSTATUS LinkIsDown(', 'static VOID ConfirmLinkDown(', 'static VOID JoinTimeout(')
  }
  'test_receive_reconnect' = @{
    SourceFile = (Join-Path $Drv 'windows\station_data.c')
    IncName    = 'receive-path-under-test.inc'
    Signatures = @('VOID RecycleRx(', 'VOID DropReady(', 'VOID RxComplete(',
                   'VOID SwifiDataBeginJoin(', 'VOID SwifiDataLink(', 'VOID AdvanceRx(')
  }
}

$results = @()
foreach ($test in $Tests) {
  $exe = Join-Path $OutDir ($test.BaseName + '.exe')
  Remove-Item $exe -Force -ErrorAction SilentlyContinue
  # shim.c is compiled separately with -fno-builtin so its memory routines are
  # not rewritten into calls to themselves.
  $shimObj = Join-Path $OutDir 'shim.obj'
  if (-not (Test-Path $shimObj) -or (Get-Item $Shim).LastWriteTime -gt (Get-Item $shimObj).LastWriteTime) {
    & "$Llvm\clang.exe" @common -fno-builtin -c $Shim -o $shimObj
    if ($LASTEXITCODE -ne 0) { throw 'shim.c failed to compile' }
  }
  $chkstkObj = Join-Path $OutDir 'chkstk.obj'
  if (-not (Test-Path $chkstkObj) -or (Get-Item $Chkstk).LastWriteTime -gt (Get-Item $chkstkObj).LastWriteTime) {
    & "$Llvm\clang.exe" @common -c $Chkstk -o $chkstkObj
    if ($LASTEXITCODE -ne 0) { throw 'chkstk.c failed to compile' }
  }

  $extraInclude = @()
  if ($Slices.ContainsKey($test.BaseName)) {
    $slice = $Slices[$test.BaseName]
    $text = Get-Content -Raw -LiteralPath $slice.SourceFile
    $parts = foreach ($signature in $slice.Signatures) { Get-FunctionSlice $text $signature }
    Set-Content -LiteralPath (Join-Path $OutDir $slice.IncName) -Value (($parts -join "`n`n") + "`n") -NoNewline
    $extraInclude = @('-I', $OutDir)
  }

  $compileArgs = $common + $extraInclude + @('-fno-builtin-memcpy', '-fno-builtin-memset', '-fno-builtin-memmove') +
    @($test.FullName) + $CoreSources + @($shimObj) + @($chkstkObj) + $link + @('-o', $exe)
  $compileOutput = & "$Llvm\clang.exe" @compileArgs 2>&1
  if ($LASTEXITCODE -ne 0) {
    $results += [pscustomobject]@{ Test = $test.BaseName; Result = 'COMPILE-FAIL'; Detail = ($compileOutput -join ' ') }
    continue
  }

  if ($NoRun) { $results += [pscustomobject]@{ Test = $test.BaseName; Result = 'BUILT'; Detail = $exe }; continue }

  $output = & $exe 2>&1
  $code = $LASTEXITCODE
  $results += [pscustomobject]@{
    Test   = $test.BaseName
    Result = $(if ($code -eq 0) { 'PASS' } else { "FAIL($code)" })
    Detail = ($output -join ' ').Trim()
  }
}

$results | Format-Table -AutoSize -Wrap | Out-String -Width 200
$failed = @($results | Where-Object { $_.Result -notin 'PASS', 'BUILT' })
"--- {0} test(s): {1} passed, {2} failed ---" -f $results.Count, ($results.Count - $failed.Count), $failed.Count
if ($failed.Count) { exit 1 }
