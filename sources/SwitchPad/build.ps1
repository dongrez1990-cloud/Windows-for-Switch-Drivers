# SwitchPad offline build: local JDK 17 + Android SDK 34, no Gradle, no network.
#   usage: powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1
#
# NOTE: keep this file ASCII-only. Windows PowerShell 5.1 reads .ps1 as ANSI,
# so non-ASCII text breaks parsing unless the file carries a UTF-8 BOM.
param(
    [string]$Kit = 'D:\paopao\.androidkit'
)

# Native tools write progress banners to stderr, which PowerShell 5.1 turns into
# terminating errors under 'Stop'. Every step below therefore checks $LASTEXITCODE
# explicitly instead of relying on the error action preference.
$ErrorActionPreference = 'Continue'

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$sdk  = Join-Path $Kit 'sdk'
$bt   = Join-Path $sdk 'build-tools\34.0.0'
$aj   = Join-Path $sdk 'platforms\android-34\android.jar'
$jdk  = Join-Path $Kit 'jdk'

$javac     = Join-Path $jdk 'bin\javac.exe'
$jar       = Join-Path $jdk 'bin\jar.exe'
$keytool   = Join-Path $jdk 'bin\keytool.exe'
$aapt2     = Join-Path $bt 'aapt2.exe'
$d8        = Join-Path $bt 'd8.bat'
$zipalign  = Join-Path $bt 'zipalign.exe'
$apksigner = Join-Path $bt 'apksigner.bat'

foreach ($tool in @($javac, $jar, $keytool, $aapt2, $d8, $zipalign, $apksigner, $aj)) {
    if (-not (Test-Path $tool)) { throw "missing build tool: $tool" }
}

$env:JAVA_HOME = $jdk
$env:PATH = (Join-Path $jdk 'bin') + ';' + $bt + ';' + $env:PATH

$build   = Join-Path $root 'build'
$gen     = Join-Path $build 'gen'
$classes = Join-Path $build 'classes'
$dexDir  = Join-Path $build 'dex'
$ksDir   = Join-Path $root 'keystore'
$apkOut  = Join-Path $root 'SwitchPad.apk'

if (Test-Path $build) { Remove-Item -Recurse -Force $build }
New-Item -ItemType Directory -Force -Path $gen, $classes, $dexDir, $ksDir | Out-Null

function Step($n, $msg) { Write-Host "==> [$n/6] $msg" -ForegroundColor Cyan }

# ---------------------------------------------------------------- 1. resources
Step 1 'aapt2 compile'
& $aapt2 compile --dir (Join-Path $root 'app\res') -o (Join-Path $build 'res.zip')
if ($LASTEXITCODE -ne 0) { throw 'aapt2 compile failed' }

# ---------------------------------------------------------------- 2. link + R.java
Step 2 'aapt2 link'
& $aapt2 link `
    -o (Join-Path $build 'base.apk') `
    -I $aj `
    --manifest (Join-Path $root 'app\AndroidManifest.xml') `
    -R (Join-Path $build 'res.zip') `
    --java $gen `
    --auto-add-overlay `
    --min-sdk-version 28 `
    --target-sdk-version 34 `
    --version-code 1 `
    --version-name '1.0'
if ($LASTEXITCODE -ne 0) { throw 'aapt2 link failed' }

# ---------------------------------------------------------------- 3. javac
Step 3 'javac'
$srcs = @()
$srcs += Get-ChildItem -Recurse -File (Join-Path $root 'app\src') -Filter *.java | ForEach-Object { $_.FullName }
$srcs += Get-ChildItem -Recurse -File $gen -Filter *.java | ForEach-Object { $_.FullName }
Write-Host ("    {0} source files" -f $srcs.Count)
$javacLog = Join-Path $build 'javac.log'
$javacCode = 0
& {
    # 抑制 stderr 横幅（Picked up JAVA_TOOL_OPTIONS）被 PowerShell 当成错误记录打印
    $ErrorActionPreference = 'SilentlyContinue'
    $env:JAVA_TOOL_OPTIONS = '-Duser.language=en -Duser.country=US'
    & $javac -encoding UTF-8 -source 8 -target 8 -nowarn -bootclasspath $aj -d $classes $srcs *> $javacLog
    $script:javacCode = $LASTEXITCODE
    Remove-Item Env:\JAVA_TOOL_OPTIONS -ErrorAction SilentlyContinue
}
Get-Content $javacLog -ErrorAction SilentlyContinue |
    Where-Object { $_ -notmatch 'Picked up' -and $_ -notmatch '^Note:' -and $_ -notmatch 'bootstrap class path' -and $_.Trim() -ne '' -and $_ -notmatch 'CategoryInfo|FullyQualifiedErrorId|^\s*\+' } |
    ForEach-Object { Write-Host "    $_" }
if ($javacCode -ne 0) { throw 'javac failed' }

# ---------------------------------------------------------------- 4. dex
Step 4 'd8'
& $jar cf (Join-Path $build 'classes.jar') -C $classes .
if ($LASTEXITCODE -ne 0) { throw 'jar failed' }
& $d8 --release --min-api 28 --lib $aj --output $dexDir (Join-Path $build 'classes.jar')
if ($LASTEXITCODE -ne 0) { throw 'd8 failed' }
if (-not (Test-Path (Join-Path $dexDir 'classes.dex'))) { throw 'classes.dex missing' }

# ---------------------------------------------------------------- 5. package
Step 5 'package apk (resources + classes.dex)'
$unsigned = Join-Path $build 'app-unsigned.apk'
Copy-Item (Join-Path $build 'base.apk') $unsigned

Add-Type -AssemblyName System.IO.Compression.FileSystem | Out-Null
$zip = [System.IO.Compression.ZipFile]::Open($unsigned, 'Update')
try {
    $dexPath = Join-Path $dexDir 'classes.dex'
    [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
        $zip, $dexPath, 'classes.dex', [System.IO.Compression.CompressionLevel]::NoCompression) | Out-Null
} finally {
    $zip.Dispose()
}

$aligned = Join-Path $build 'app-aligned.apk'
& $zipalign -p -f 4 $unsigned $aligned
if ($LASTEXITCODE -ne 0) { throw 'zipalign failed' }

# ---------------------------------------------------------------- 6. sign
Step 6 'sign'
$ks = Join-Path $ksDir 'switchpad.keystore'
if (-not (Test-Path $ks)) {
    & $keytool -genkeypair -keystore $ks -alias switchpad -keyalg RSA -keysize 2048 `
        -validity 10000 -storepass switchpad -keypass switchpad `
        -dname 'CN=SwitchPad, O=SwitchPad, C=CN' *> (Join-Path $build 'keytool.log')
    if ($LASTEXITCODE -ne 0) { throw 'keytool failed' }
}

if (Test-Path $apkOut) { Remove-Item -Force $apkOut }
& $apksigner sign --ks $ks --ks-pass 'pass:switchpad' --key-pass 'pass:switchpad' --out $apkOut $aligned
if ($LASTEXITCODE -ne 0) { throw 'apksigner failed' }

& $apksigner verify --print-certs $apkOut | Select-Object -First 2 | ForEach-Object { Write-Host "    $_" }

Write-Host ''
& $aapt2 dump badging $apkOut |
    Where-Object { $_ -match 'package:|sdkVersion|targetSdkVersion|launchable-activity' } |
    ForEach-Object { Write-Host "    $_" }

$size = (Get-Item $apkOut).Length / 1KB
Write-Host ''
Write-Host ("OK  {0}  ({1:N1} KB)" -f $apkOut, $size) -ForegroundColor Green
