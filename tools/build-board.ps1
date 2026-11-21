# 编译上游 tegra-x1-wddm 的 board 代码（platform + core），验证工具链可用性。
# 用法: powershell -NoProfile -File build-board.ps1
# 说明:
#  - 源码来自 erista-gpu\src\mesa-switch-woa（GitHub 克隆）
#  - nouveau 头来自 erista-gpu\src\mesa-switch（稀疏克隆，src/nouveau）
#  - 已知缺口: core/command_stream.c 需要 src/vulkan 下的 nvk_mme_slots.h；
#              core/report.c 需要 mesm src/util（可扩 sparse-checkout 补齐）
$ErrorActionPreference = 'Stop'
$root = 'D:\paopao\erista-gpu\src\mesa-switch-woa\tegra-x1-wddm'
$woa = 'D:\paopao\erista-gpu\src\mesa-switch-woa'
$nvk = 'D:\paopao\erista-gpu\src\mesa-switch\src'
$shim = 'D:\paopao\WoA\erista-gpu\tests\shim'
$clang = 'C:\Program Files\LLVM\bin\clang.exe'
$ar = 'C:\Program Files\LLVM\bin\llvm-ar.exe'
$outDir = 'D:\paopao\WoA\erista-gpu\tests\board-obj'
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

function Compile([string]$src, [string]$name) {
    if (-not (Test-Path $src)) { return $false }
    $o = Join-Path $outDir $name
    $r = & $clang --target=aarch64-w64-windows-gnu -std=c11 -ffreestanding -O2 -Wall `
        -I $root -I "$woa\src" -I $nvk -I "$nvk\nouveau\headers" -I $shim -c $src -o $o 2>&1
    if ($LASTEXITCODE -eq 0) { return $true }
    Write-Host ("FAIL " + (Split-Path $src -Leaf))
    ($r | Select-Object -First 3) | ForEach-Object { Write-Host ("   " + $_) }
    return $false
}

$objs = @()
$ok = 0; $fail = 0
foreach ($f in Get-ChildItem "$root\platform" -Filter *.c) {
    if ($f.Name -eq 't214_nvdec.c') { continue }   # 需要 <string.h>，见 README
    if (Compile $f.FullName ($f.BaseName + '.o')) { $ok++; $objs += (Join-Path $outDir ($f.BaseName + '.o')) } else { $fail++ }
}
foreach ($f in Get-ChildItem "$root\core" -Filter *.c) {
    if (Compile $f.FullName ('core-' + $f.BaseName + '.o')) { $ok++; $objs += (Join-Path $outDir ('core-' + $f.BaseName + '.o')) } else { $fail++ }
}
Write-Host "board objects: ok=$ok fail=$fail"
if ($objs.Count -gt 0) {
    & $ar rcs 'D:\paopao\WoA\erista-gpu\tests\libboard-arm64.a' @objs
    Write-Host ("archive: " + (Get-Item 'D:\paopao\WoA\erista-gpu\tests\libboard-arm64.a').Length + " bytes")
}
