# Runs the JVM unit tests (no Android SDK needed).
$ErrorActionPreference = 'Continue'
$kit = 'D:\paopao\.androidkit'
$proj = $PSScriptRoot | Split-Path -Parent
$src = "$proj\app\src\main\java\com\paopao\switchkey"
$out = "$proj\app\build\test"

New-Item -ItemType Directory -Force -Path $out | Out-Null
& "$kit\jdk\bin\javac.exe" -encoding UTF-8 -d $out `
    "$src\HidDescriptors.java" "$src\KeyState.java" "$src\PointerPump.java" `
    "$proj\tools\HidDescriptorTest.java" "$proj\tools\KeyStateTest.java" `
    "$proj\tools\PointerPumpTest.java"
if ($LASTEXITCODE -ne 0) { Write-Host "COMPILE FAILED"; exit 1 }

$fail = 0
foreach ($t in @("HidDescriptorTest", "KeyStateTest", "PointerPumpTest")) {
    Write-Host "=== $t ==="
    & "$kit\jdk\bin\java.exe" -cp $out "com.paopao.switchkey.$t"
    if ($LASTEXITCODE -ne 0) { $fail++ }
}
if ($fail -gt 0) { Write-Host "TESTS FAILED: $fail"; exit 1 }
Write-Host "ALL TESTS PASSED"
