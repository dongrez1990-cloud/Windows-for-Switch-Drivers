# Build the patched Erista (T210) switchfan KMDF driver: fan/backlight/PMIC bridge
# plus the MAX77620 raw-register path used to drive VDD_GPU's enable pin.
param(
    [string]$Out = 'D:\paopao\WoA\erista-gpu\switchfan-t210\build',
    [string]$Version = '1.0.0.31'
)
$ErrorActionPreference = 'Stop'
$src = 'D:\paopao\WoA\erista-gpu\switchfan-t210'
$kits = 'C:\Program Files (x86)\Windows Kits\10'
$wdk = "$kits\Include\10.0.19041.0"
$kmLib = "$kits\Lib\10.0.19041.0\km\arm64"
$kmdfInc = "$kits\Include\wdf\kmdf\1.15"
$kmdfLib = "$kits\Lib\wdf\kmdf\arm64\1.15"
$llvm = 'C:\Program Files\LLVM\bin'
$inf2cat = 'D:\paopao\WoA\erista-gpu\wdk\extracted\c\bin\10.0.28000.0\x86\Inf2Cat.exe'

New-Item -ItemType Directory -Path $Out -Force | Out-Null

$cflags = @(
    '--target=aarch64-pc-windows-msvc', '/c', '/kernel', '/GS-', '/Gy', '/Zl', '/O2', '/W4', '/WX',
    '-Wno-unused-parameter', '-Wno-microsoft-anon-tag', '-Wno-ignored-pragma-intrinsic',
    '-Wno-pragma-pack', '-Wno-invalid-token-paste', '-Wno-unknown-pragmas', '-Wno-ignored-attributes',
    '-Wno-nonportable-include-path', '-Wno-microsoft-enum-value', '-Wno-unused-function',
    '/D_ARM64_', '/DARM64', '/D_WIN64', '/DNTDDI_VERSION=0x0A000008', '/D_WIN32_WINNT=0x0A00', '/DWINVER=0x0A00',
    '/DKMDF_VERSION_MAJOR=1', '/DKMDF_VERSION_MINOR=15', '/DPOOL_NX_OPTIN=1',
    "/I$src", "/imsvc$wdk\km\crt", "/imsvc$wdk\km", "/imsvc$wdk\shared", "/imsvc$kmdfInc", "/imsvc$wdk\km\spb\1.0"
)

$objs = @()
foreach ($s in @("$src\switchfan.c", 'D:\paopao\WoA\Windows-for-Switch-Drivers\tools\kmdf_support.c')) {
    if (-not (Test-Path $s)) { throw "missing $s" }
    $obj = Join-Path $Out ([IO.Path]::GetFileNameWithoutExtension($s) + '.obj')
    & "$llvm\clang-cl.exe" @cflags $s "/Fo$obj"
    if ($LASTEXITCODE -ne 0) { throw "compile failed: $s" }
    $objs += $obj
    "compiled: $([IO.Path]::GetFileName($s))"
}

$sys = Join-Path $Out 'switchfan.sys'
$libs = @(
    "$kmdfLib\WdfDriverEntry.lib", "$kmdfLib\WdfLdr.lib",
    "$kmLib\ntoskrnl.lib", "$kmLib\hal.lib", "$kmLib\ntstrsafe.lib", "$kmLib\bufferoverflowfastfailk.lib"
)
& "$llvm\lld-link.exe" /nologo /driver '/subsystem:native,10.0' /machine:arm64 /entry:FxDriverEntry `
    /nodefaultlib /release /opt:ref /opt:icf /tsaware:no `
    '/section:.text,ERP' '/section:.rdata,RP' '/section:.data,RWP' '/section:.pdata,RP' '/section:INIT,ERD' `
    "/out:$sys" @objs @libs | Select-Object -Last 12
if ($LASTEXITCODE -ne 0) { throw 'link failed' }
"sys: {0:N0} B" -f (Get-Item $sys).Length

Copy-Item "$src\switchfan.inf" "$Out\switchfan.inf" -Force
$inf = Get-Content "$Out\switchfan.inf" -Raw
$inf = $inf -replace 'DriverVer\s*=.*', ("DriverVer   = 11/18/2026," + $Version)
Set-Content "$Out\switchfan.inf" $inf -Encoding ascii
Remove-Item "$Out\switchfan.cat" -Force -ErrorAction SilentlyContinue
& $inf2cat "/driver:$Out" /os:10_RS5_ARM64 | Select-Object -Last 1
$cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq 'CN=SwitchWOA Test Driver Signing' -and $_.HasPrivateKey } | Select-Object -First 1
$null = Set-AuthenticodeSignature -FilePath $sys -Certificate $cert -HashAlgorithm SHA256
$null = Set-AuthenticodeSignature -FilePath "$Out\switchfan.cat" -Certificate $cert -HashAlgorithm SHA256
"package: $(Select-String "$Out\switchfan.inf" -Pattern 'DriverVer').Line"
"sys sha: " + (Get-FileHash $sys -Algorithm SHA256).Hash.Substring(0,16)
