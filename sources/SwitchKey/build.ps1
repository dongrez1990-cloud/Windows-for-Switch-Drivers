# Minimal APK build without Gradle / Android Studio.
#   aapt2 compile -> aapt2 link -> javac -> d8 -> merge dex -> zipalign -> apksigner
# Toolchain is expected at D:\paopao\.androidkit (see .androidkit\setup.ps1).
# Kept ASCII-only on purpose: Windows PowerShell 5.1 reads .ps1 as ANSI without BOM.

$ErrorActionPreference = 'Continue'

$kit   = 'D:\paopao\.androidkit'
$sdk   = "$kit\sdk"
$bt    = "$sdk\build-tools\34.0.0"
$jar   = "$sdk\platforms\android-34\android.jar"
$proj  = $PSScriptRoot
$src   = "$proj\app\src\main"
$build = "$proj\app\build"
$apk   = "$proj\SwitchKey.apk"

$env:JAVA_HOME = "$kit\jdk"
$env:PATH = "$kit\jdk\bin;$bt;$env:PATH"

foreach ($p in @("$bt\aapt2.exe", $jar, "$kit\jdk\bin\javac.exe")) {
    if (-not (Test-Path $p)) { Write-Host "MISSING TOOLCHAIN FILE: $p"; exit 1 }
}

function Run-Tool {
    param([string]$Name, [string]$Exe, [string[]]$ArgList)
    Write-Host "==> $Name"
    $out = & $Exe @ArgList 2>&1
    $code = $LASTEXITCODE
    $out | Where-Object { $_ -notmatch 'bootstrap class path|obsolete|deprecat|Picked up JAVA_TOOL_OPTIONS|^Note:' } | Out-Host
    if ($code -ne 0) { Write-Host "FAILED: $Name (exit $code)"; exit $code }
}

Write-Host "==> clean"
if (Test-Path $build) { Remove-Item -Recurse -Force $build }
New-Item -ItemType Directory -Force -Path "$build\gen", "$build\classes", "$build\dex" | Out-Null

Run-Tool -Name "icon" -Exe "node" -ArgList @("$proj\tools\gen-icon.mjs", "$src\res\mipmap\ic_launcher.png")

Run-Tool -Name "aapt2 compile" -Exe "$bt\aapt2.exe" `
    -ArgList @("compile", "--dir", "$src\res", "-o", "$build\res.zip")

Run-Tool -Name "aapt2 link" -Exe "$bt\aapt2.exe" `
    -ArgList @("link", "-o", "$build\base.apk", "-I", $jar,
            "--manifest", "$src\AndroidManifest.xml",
            "-R", "$build\res.zip",
            "--java", "$build\gen",
            "--min-sdk-version", "28", "--target-sdk-version", "34",
            "--version-code", "7", "--version-name", "1.6",
            "--auto-add-overlay")

$sources = @(Get-ChildItem -Recurse -Filter *.java -Path "$src\java" | ForEach-Object { $_.FullName })
$sources += @(Get-ChildItem -Recurse -Filter *.java -Path "$build\gen" | ForEach-Object { $_.FullName })
Write-Host "==> javac ($($sources.Count) files)"
$javacArgs = @("-encoding", "UTF-8", "-source", "8", "-target", "8", "-nowarn",
               "-classpath", $jar, "-d", "$build\classes") + $sources
Run-Tool -Name "javac" -Exe "$kit\jdk\bin\javac.exe" -ArgList $javacArgs

$classes = @(Get-ChildItem -Recurse -Filter *.class -Path "$build\classes" | ForEach-Object { $_.FullName })
$d8Args = @("--min-api", "28", "--lib", $jar, "--output", "$build\dex") + $classes
Run-Tool -Name "d8" -Exe "$bt\d8.bat" -ArgList $d8Args

Run-Tool -Name "merge classes.dex" -Exe "node" `
    -ArgList @("$proj\tools\zipadd.mjs", "$build\base.apk", "$build\unsigned.apk",
            "$build\dex\classes.dex", "classes.dex")

Run-Tool -Name "zipalign" -Exe "$bt\zipalign.exe" `
    -ArgList @("-f", "-p", "4", "$build\unsigned.apk", "$build\aligned.apk")

$ks = "$proj\debug.keystore"
if (-not (Test-Path $ks)) {
    Write-Host "==> create debug keystore"
    & "$kit\jdk\bin\keytool.exe" -genkeypair -keystore $ks -storepass android -keypass android `
        -alias androiddebugkey -dname "CN=Android Debug,O=Android,C=US" `
        -keyalg RSA -keysize 2048 -validity 10000 -storetype PKCS12 2>&1 | Out-Null
}

if (Test-Path $apk) { Remove-Item -Force $apk }
Run-Tool -Name "apksigner" -Exe "$bt\apksigner.bat" `
    -ArgList @("sign", "--ks", $ks, "--ks-pass", "pass:android", "--key-pass", "pass:android",
            "--ks-key-alias", "androiddebugkey", "--out", $apk, "$build\aligned.apk")

Write-Host "==> verify"
& "$bt\apksigner.bat" verify --print-certs $apk 2>&1 | Select-Object -First 3 | Out-Host
& "$bt\aapt2.exe" dump badging $apk 2>&1 |
    Select-String -Pattern "package:|sdkVersion|targetSdkVersion|launchable-activity" |
    Select-Object -First 6 | Out-Host

$info = Get-Item $apk
Write-Host ""
Write-Host "BUILD OK -> $($info.FullName)  ($([math]::Round($info.Length/1KB,1)) KB)"
