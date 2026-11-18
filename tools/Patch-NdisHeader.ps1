<#
.SYNOPSIS
  Derive a clang-compatible copy of the WDK's ndis.h for one SDK version.

.DESCRIPTION
  The WDK writes

    #define NDIS_API_VERSION_AVAILABLE(major,minor) ((((0x ## major) << 8) + (0x ## minor)) >= NDIS_MIN_API)

  C says a macro argument next to ## is not expanded, so clang sees the literal
  token 0xNDIS_MINIPORT_MAJOR_VERSION and fails with "invalid suffix". MSVC
  expands first and therefore compiles. The fix is a two-level paste, which
  Build-Driver.ps1 injects with -IncludePaths (a plain /I path is searched
  before /imsvc, so it shadows the SDK header).

  The copy is derived from the installed header rather than hand-edited, so it
  stays structurally identical to the SDK version being compiled against --
  mixing a 19041 ndis.h into a 22621 build would mean mismatched structure
  definitions under the newer NetAdapterCx/WiFiCx headers.

.EXAMPLE
  .\Patch-NdisHeader.ps1 -WdkVersion 10.0.22621.0
#>
param(
  [Parameter(Mandatory = $true)][string]$WdkVersion,
  [string]$OutDir
)

$ErrorActionPreference = 'Stop'
$Kits = "${env:ProgramFiles(x86)}\Windows Kits\10"
$src = Join-Path $Kits "Include\$WdkVersion\km\ndis.h"
if (-not (Test-Path $src)) { throw "Missing source header: $src" }

if (-not $OutDir) { $OutDir = Join-Path $PSScriptRoot "wdk-patch\$($WdkVersion.Split('.')[2])" }
New-Item -ItemType Directory -Force $OutDir | Out-Null
$dst = Join-Path $OutDir 'ndis.h'

$old = '#define NDIS_API_VERSION_AVAILABLE(major,minor) ((((0x ## major) << 8) + (0x ## minor)) >= NDIS_MIN_API)'
$new = @(
  '#define NDIS_HEXPASTE_(x) 0x##x',
  '#define NDIS_HEXPASTE(x)  NDIS_HEXPASTE_(x)',
  '#define NDIS_API_VERSION_AVAILABLE(major,minor) ((((NDIS_HEXPASTE(major)) << 8) + (NDIS_HEXPASTE(minor))) >= NDIS_MIN_API)'
) -join "`r`n"

$text = [IO.File]::ReadAllText($src)
$n = ([regex]::Matches($text, [regex]::Escape($old))).Count

# The 10.0.26100 kit already routes the arguments through an intermediate macro,
# which is exactly the two-level expansion this patch adds. Nothing to do there.
if ($n -eq 0 -and $text.Contains('NDIS_DETAIL_NDIS_API_VERSION_AVAILABLE_')) {
  Write-Host "not needed: $src already expands the version arguments (kit >= 10.0.26100)"
  if (Test-Path $dst) { Remove-Item $dst -Force }
  return
}
if ($n -ne 1) { throw "expected exactly 1 macro definition in $src, found $n" }

$text = $text.Replace($old, $new)
[IO.File]::WriteAllText($dst, $text, (New-Object System.Text.UTF8Encoding($false)))
Write-Host "patched: $dst (from $src)"
