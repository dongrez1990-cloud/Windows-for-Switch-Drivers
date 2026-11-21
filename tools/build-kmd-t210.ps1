# Build the T210 (Erista) render miniport locally with clang-cl + lld-link.
#
# Mirrors tegra-x1-wddm/tools/build.py (MCDM render package path) but swaps the
# T214 platform sources for the working-tree platform/t210 port.
#
#   powershell -NoProfile -File build-kmd-t210.ps1 [-Only <name>] [-KeepGoing]
#
# Output: <out>\tegragpurender.sys (+ TegraGpuCore.lib, objects)

param(
    [string]$Out = 'D:\paopao\WoA\erista-gpu\kmd-build',
    [string]$Only = '',
    [switch]$KeepGoing
)

$ErrorActionPreference = 'Continue'
$root  = 'D:\paopao\erista-gpu\src\mesa-switch-woa\tegra-x1-wddm'
$mesa  = 'D:\paopao\erista-gpu\src\mesa-switch-woa'
$wdk   = 'C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0'
$kmLib = 'C:\Program Files (x86)\Windows Kits\10\Lib\10.0.26100.0\km\ARM64'
$clangCl = 'C:\Program Files\LLVM\bin\clang-cl.exe'
$lib     = 'C:\Program Files\LLVM\bin\llvm-lib.exe'
$link    = 'C:\Program Files\LLVM\bin\lld-link.exe'

New-Item -ItemType Directory -Path $Out -Force | Out-Null

$includes = @(
    "$mesa\src\nouveau\vulkan\nvkmd\windows",
    "$mesa\src",
    "$mesa\src\nouveau\headers",
    "$mesa\src\nouveau\headers\nvidia\classes",
    "$mesa\src\nouveau\nvdec"
)
$flags = @(
    '--target=aarch64-pc-windows-msvc', '/nologo', '/c', '/kernel',
    '/GS-', '/Gy', '/Zl', '/O2', '/W4', '/WX', '/std:c11',
    '/clang:-ffreestanding', '-Wno-microsoft-anon-tag', '-Wno-pragma-pack',
    '-Wno-ignored-pragma-intrinsic', '-Wno-nonportable-include-path',
    '/D_ARM64_', '/DARM64', '/D_WIN64', '/D_WIN32_WINNT=0x0A00',
    '/DNTDDI_VERSION=0x0A00000C', '/DNDEBUG',
    '/DTG_GPU_RENDER_KHZ=460800',
    '/DDXGKDDI_INTERFACE_VERSION=DXGKDDI_INTERFACE_VERSION_WDDM2_6',
    "/I$root", "/I$root\windows", "/I$root\core", "/I$root\platform", "/I$root\platform\t210"
) + ($includes | ForEach-Object { "/I$_" }) + @(
    "/imsvc$wdk\km\crt", "/imsvc$wdk\km", "/imsvc$wdk\shared", "/imsvc$wdk\ucrt"
)

# T210 platform sources replace the T214 ones where the port provides them.
$coreSources = @(
    'core\gm20b_mmu.c', 'core\report.c', 'core\gpfifo.c', 'core\fault.c',
    'core\execute.c', 'core\submission.c', 'core\channel.c',
    'core\command_stream.c', 'core\copy_stream.c', 'core\present_halfturn.c',
    'core\paging_dma.c', 'core\present_blit.c',
    'core\gr_firmware.c', 'core\acr_firmware.c', 'core\gr_image.c',
    'core\gr_submit.c', 'core\gr_clear.c', 'core\gr_draw.c'
)
$platformSources = @(
    'platform\t210\t210_mmu.c',           # may not exist -> skipped below
    'platform\t210\t210_frontend.c',
    'platform\t210\t210_gr.c',
    'platform\t210\t210_acr.c',
    'platform\t210\t210_pmu.c',
    'platform\t210\t210_power.c',
    'platform\t210\t210_clock.c',
    'platform\t210\t210_opp.c',
    'platform\t214_mmu.c',
    'platform\t214_gr_context.c',
    'platform\t214_interrupt.c',
    'platform\t214_dc.c',
    'platform\t214_nvdec.c'
)
$windowsSources = @(
    'windows\resources.c', 'windows\panel.c', 'windows\panel_vidpn.c', 'windows\panel_scanout.c',
    'windows\private_memory.c', 'windows\paging.c',
    'windows\table_segment.c', 'windows\root_table.c',
    'windows\paging_buffer.c', 'windows\paging_dispatch.c', 'windows\paging_submit.c',
    'windows\context.c', 'windows\device_lifetime.c', 'windows\internal_va.c',
    'windows\submit_worker.c', 'windows\graphics.c', 'windows\hardware.c',
    'windows\work_queue.c', 'windows\allocation.c', 'windows\query.c', 'windows\adapter.c',
    'windows\submit_ddi.c', 'windows\present.c', 'windows\command_snapshot.c',
    'windows\recovery.c', 'windows\diagnostic_events.c',
    'windows\platform_io.c', 'windows\host1x_clock.c', 'windows\dvfs.c', 'windows\worker.c',
    'windows\platform_adapter.c', 'windows\gr_firmware_io.c',
    'windows\reset_backend.c', 'windows\adapter_reset.c',
    'windows\nvdec_adapter.c', 'windows\nvdec_backend.c', 'windows\nvdec_memory.c'
)

$objs = New-Object System.Collections.Generic.List[string]
$failed = New-Object System.Collections.Generic.List[string]

function Compile-Source([string]$rel) {
    if ([IO.Path]::IsPathRooted($rel)) { $src = $rel } else { $src = Join-Path $root $rel }
    if (-not (Test-Path $src)) { Write-Host "SKIP (missing) $rel"; return $false }
    $name = ($rel -replace '[\\/]', '-') -replace '\.c$', '.obj'
    $obj = Join-Path $Out $name
    if ($Only -and $name -notmatch [regex]::Escape($Only)) { return $true }
    & $clangCl @flags "/Fo$obj" -- $src 2>&1 | ForEach-Object { $_ } | Select-Object -Last 8 | Where-Object { $_ -match 'error|warning' } | ForEach-Object { Write-Host ("   " + $_) }
    if ($LASTEXITCODE -ne 0) {
        Write-Host "FAIL $rel"
        $failed.Add($rel) | Out-Null
        return $false
    }
    $objs.Add($obj) | Out-Null
    return $true
}

Write-Host "=== compiling core/platform ==="
foreach ($s in ($coreSources + $platformSources)) { Compile-Source $s | Out-Null }
Write-Host "=== compiling windows ==="
foreach ($s in $windowsSources) { Compile-Source $s | Out-Null }
# mesa encoder + NVDEC core
Compile-Source "$mesa\src\nouveau\horizon\nouveau_horizon_gm20b.c" | Out-Null
foreach ($n in 'nvdec_sha256.c', 'nvdec_fw.c', 'nvdec_h264.c', 'nvdec_engine.c') {
    Compile-Source "$mesa\src\nouveau\nvdec\$n" | Out-Null
}

Write-Host ("objects: {0}  failed: {1}" -f $objs.Count, $failed.Count)
if ($failed.Count -gt 0) {
    Write-Host ("failed list: " + ($failed -join ', '))
    if (-not $KeepGoing) { exit 1 }
}

$libPath = Join-Path $Out 'TegraGpuCore.lib'
if (Test-Path $libPath) { Remove-Item $libPath -Force }
& $lib /nologo /machine:arm64 "/out:$libPath" @objs 2>&1 | Select-Object -Last 3
if ($LASTEXITCODE -ne 0) { Write-Host 'lib failed'; exit 1 }
Write-Host ("lib: {0:N0} B" -f (Get-Item $libPath).Length)

$mini = Join-Path $Out 'render_miniport.obj'
& $clangCl @flags "/Fo$mini" -- (Join-Path $root 'windows\render_miniport.c') 2>&1 | Select-Object -Last 10
if ($LASTEXITCODE -ne 0) { Write-Host 'miniport compile failed'; exit 1 }

$sys = Join-Path $Out 'tegragpurender.sys'
& $link /nologo /driver /subsystem:native,10.0 /machine:arm64 /entry:GsDriverEntry /include:_load_config_used `
    /nodefaultlib /release /opt:ref /tsaware:no `/section:.text`,ERP `/section:.rdata`,RP `/section:.data`,RWP `
    `/section:.pdata`,RP `/section:INIT`,ERD /timestamp:0 "/out:$sys" $mini $libPath `
    (Join-Path $kmLib 'displib.lib') (Join-Path $kmLib 'ntoskrnl.lib') (Join-Path $kmLib 'hal.lib') `
    (Join-Path $kmLib 'bufferoverflowfastfailk.lib') (Join-Path $kmLib 'libcntpr.lib') 2>&1 | Select-Object -Last 20
if ($LASTEXITCODE -ne 0) { Write-Host 'link failed'; exit 1 }
Write-Host ("sys: {0:N0} B" -f (Get-Item $sys).Length)



