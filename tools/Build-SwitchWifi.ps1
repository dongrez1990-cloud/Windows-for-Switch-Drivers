<#
.SYNOPSIS
  Build and test-sign switchwifi.sys for ARM64 Windows 22621+.

.DESCRIPTION
  SwitchWifi is not a plain KMDF driver: it is a WiFiCx/NetAdapterCx client, and
  those class extensions first shipped in the Windows 11 WDK. Compiling against
  10.0.19041.0 therefore fails at `#include <wificx.h>`, which is why this build
  pins the 10.0.22621.0 headers and the 1.33 KMDF that switchwifi.inf declares.

  Three things differ from the ordinary driver build:

    1. `-WdkVersion 10.0.22621.0` moves the header and km\arm64 library root.
    2. `-IncludePaths` carries a locally patched ndis.h. The WDK writes
       `0x ## major`, which C forbids expanding, and clang -- unlike MSVC --
       obeys that rule. A plain /I path is searched before /imsvc, so the copy
       derived by ..\Patch-NdisHeader.ps1 shadows the SDK header.
    3. The class-extension stub libraries are passed by full path, because
       adapter\netadaptercxstub.lib exists once per NetAdapterCx version under
       the same file name.

.EXAMPLE
  .\Build-SwitchWifi.ps1
  .\Build-SwitchWifi.ps1 -NoSign
#>
param(
  [string]$OutDir,
  [switch]$NoSign
)

$ErrorActionPreference = 'Stop'
$Repo = Split-Path $PSScriptRoot -Parent
$Drv = Join-Path $Repo 'drivers\SwitchWifi'
$Kits = "${env:ProgramFiles(x86)}\Windows Kits\10"
$Wdk = '10.0.26100.0'
$Kmdf = '1.33'
$NetCx = '2.2'          # README: "The build uses KMDF 1.33, NetAdapterCx 2.2 and WiFiCx 1.0"
if (-not $OutDir) { $OutDir = Join-Path $Repo 'build\SwitchWifi' }

# The 22621 kit cannot build this driver at all: its WiFiCx 1.0 predates
# WIFI_STATION_CAPABILITIES_INIT and the RSNA AKM fields of
# WDI_CONNECT_PARAMETERS_CONTAINER. 26100 is the kit the driver was written for.
$NdisPatch = Join-Path (Split-Path $Repo -Parent) "tools\wdk-patch\$($Wdk.Split('.')[2])"
if (-not (Test-Path (Join-Path $NdisPatch 'ndis.h'))) {
  & (Join-Path (Split-Path $Repo -Parent) 'tools\Patch-NdisHeader.ps1') -WdkVersion $Wdk
}

$sources = @(
  Get-ChildItem (Join-Path $Drv 'windows') -Filter *.c   | Sort-Object Name | ForEach-Object { $_.FullName }
  Get-ChildItem (Join-Path $Drv 'windows') -Filter *.cpp | Sort-Object Name | ForEach-Object { $_.FullName }
  Get-ChildItem (Join-Path $Drv 'core')    -Filter *.c   | Sort-Object Name | ForEach-Object { $_.FullName }
)

$libs = @(
  "$Kits\Lib\$Wdk\km\arm64\netcx\kmdf\adapter\$NetCx\netadaptercxstub.lib",
  "$Kits\Lib\$Wdk\km\arm64\wificx\1.0\wificxstub.lib",
  "$Kits\Lib\$Wdk\km\arm64\wlan\2.0\WificxTLVGenParse.lib",
  # WificxTLVGenParse.lib is C++ built with /GS and references the ARM64 cookie
  # helpers __security_push_cookie / __security_pop_cookie. bufferoverflowfastfailk.lib
  # only carries __security_check_cookie; libcntpr.lib is the kernel C++ support
  # library that provides the pair.
  "$Kits\Lib\$Wdk\km\arm64\libcntpr.lib"
)

$includes = @(
  "$Kits\Include\$Wdk\km\netcx\kmdf\adapter\$NetCx",
  "$Kits\Include\$Wdk\km\wificx\1.0",
  "$Kits\Include\$Wdk\km\wlan\2.0",
  "$Kits\Include\$Wdk\shared\netcx\shared\1.0"
)

$build = @{
  Name         = 'switchwifi'
  Sources      = $sources
  OutDir       = $OutDir
  Inf          = Join-Path $Drv 'package\switchwifi.inf'
  KmdfVersion  = $Kmdf
  WdkVersion   = $Wdk
  NtddiVersion = '0x0A00000C'   # NTDDI_WIN10_NI: NetAdapterCx 2.2 requires >= FE, INF targets 22621
  Libs         = $libs
  ExtraIncludes = $includes
  Inf2CatOs    = @('10_NI_ARM64')   # the INF decorates NTarm64.10.0...22621
}
# Only pass the shadow path when a patched copy actually exists (it does not for
# kits >= 26100, which already expand the NDIS version arguments correctly).
if (Test-Path (Join-Path $NdisPatch 'ndis.h')) { $build.IncludePaths = @($NdisPatch) }
if ($NoSign) { $build.NoSign = $true }

& (Join-Path $PSScriptRoot 'Build-Driver.ps1') @build
