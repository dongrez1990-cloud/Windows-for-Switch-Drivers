# Build the ARM64 screen-path CPU benchmark for the console.
#
#   powershell -NoProfile -File build-screenbench.ps1
#
# Output: <out>\screenbench.exe (ARM64, aarch64-pc-windows-msvc, /nodefaultlib,
# Kernel32 only). No driver, no signing, no installation: scp + run.
param(
    [string]$Out = 'D:\paopao\WoA\erista-gpu\tests\arm64-bin',
    [string]$Src = 'D:\paopao\WoA\erista-gpu\tests\screenbench.c'
)
$ErrorActionPreference = 'Stop'
$llvm = 'C:\Program Files\LLVM\bin'
$kits = 'C:\Program Files (x86)\Windows Kits\10'
$ver  = '10.0.26100.0'
New-Item -ItemType Directory -Path $Out -Force | Out-Null

$flags = @(
    '--target=aarch64-pc-windows-msvc', '/std:c11', '/O2', '/nologo', '/GS-',
    '/clang:-Wno-unused-parameter',
    "/imsvc$kits\Include\$ver\ucrt", "/imsvc$kits\Include\$ver\um", "/imsvc$kits\Include\$ver\shared"
)
$link = @(
    '/nodefaultlib', '/subsystem:console', '/entry:main_entry', '/machine:arm64',
    "$kits\Lib\$ver\um\arm64\kernel32.lib"
)
$exe = Join-Path $Out 'screenbench.exe'
Remove-Item $exe -Force -ErrorAction SilentlyContinue
& "$llvm\clang-cl.exe" --target=aarch64-pc-windows-msvc /c /std:c11 /O2 /nologo /GS- /clang:-Wno-unused-parameter `
    "/imsvc$kits\Include\$ver\ucrt" "/imsvc$kits\Include\$ver\um" "/imsvc$kits\Include\$ver\shared" `
    $Src "/Fo$Out\screenbench.obj" 2>&1 | Select-Object -Last 8
if ($LASTEXITCODE -ne 0) { throw 'compile failed' }
& "$llvm\lld-link.exe" @link "/out:$exe" "$Out\screenbench.obj" 2>&1 | Select-Object -Last 8
if (-not (Test-Path $exe)) { throw 'build failed' }
"built: $exe  ({0:N0} B)" -f (Get-Item $exe).Length
"sha256: " + (Get-FileHash $exe -Algorithm SHA256).Hash
