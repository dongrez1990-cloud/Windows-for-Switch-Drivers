<#
.SYNOPSIS
  Install the Erista (original Switch, T210) Windows driver set from this bundle.

.DESCRIPTION
  Does the whole install in the right order and then verifies it:

    1. imports the three signing certificates into LocalMachine\Root and
       LocalMachine\TrustedPublisher (skip with -SkipCerts);
    2. installs every package under <bundle>\packages with
       `pnputil /add-driver <inf> /install`, infrastructure first and Wi-Fi last,
       because installing the Wi-Fi driver restarts the network adapter;
    3. prints a table of expected vs. actually running driver versions;
    4. writes install.log next to this script.

  Run it ELEVATED, on the Switch itself. It does not need internet access.

.PARAMETER PackageRoot
  Directory holding the <driver>-<version> folders. Defaults to ..\packages.

.PARAMETER SkipCerts
  Do not touch the certificate stores (use when you already trust them).

.PARAMETER Only
  Only install these drivers, e.g. -Only switchwifi,switchfan

.PARAMETER DryRun
  Print the commands and the verification table without changing anything.

.EXAMPLE
  .\Install-SwitchDrivers.ps1 -DryRun          # look before you leap
  .\Install-SwitchDrivers.ps1                  # full install
  .\Install-SwitchDrivers.ps1 -Only switchwifi # just the Wi-Fi driver
#>
[CmdletBinding()]
param(
    [string]$PackageRoot,
    [switch]$SkipCerts,
    [string[]]$Only,
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
if (-not $PackageRoot) { $PackageRoot = Join-Path (Split-Path $here -Parent) 'packages' }
$log = Join-Path $here 'install.log'
$CertDir = Join-Path $here 'certs'

# Install order: buses and platform first, GNSS-free peripherals next, Bluetooth
# after the UART host, Wi-Fi absolutely last because it drops the network.
$Order = @(
    'tegra210gpio', 'tegra210i2c', 'tegra210sdhc',
    'switchtouch', 'switchsensor', 'switchsio', 'switchbattery', 'switchrtc',
    'switchpwrbtn', 'switchfan', 'switchaudio', 'switchbt',
    'switchjoycon', 'switchwifi'
)

# What each driver should report once installed (matches the author's console).
$Expect = [ordered]@{
    'switchwifi'    = '0.7.4.0'
    'switchjoycon'  = '0.3.20.0'
    'switchbt'      = '1.0.0.19'
    'switchfan'     = '1.0.0.22'
    'switchaudio'   = '1.0.0.17'
    'switchbattery' = '1.0.0.11'
    'switchpwrbtn'  = '1.0.0.4'
    'switchrtc'     = '1.0.0.2'
    'switchsio'     = '0.2.3.0'
    'switchsensor'  = '1.0.0.1'
    'switchtouch'   = '1.0.0.4'
    'tegra210gpio'  = '1.0.0.4'
    'tegra210i2c'   = '1.0.0.5'
    'nvtsdhc'       = '0.4.0.1'
}

function Say([string]$Message) {
    $line = "[{0}] {1}" -f (Get-Date -Format 'HH:mm:ss'), $Message
    Write-Host $line
    Add-Content -Path $log -Value $line -Encoding utf8
}

function Find-Package([string]$Driver) {
    if (-not (Test-Path $PackageRoot)) { return $null }
    # Only real version folders. "switchwifi-rollback-0.7.9.0-as-0.7.0.0" is the
    # downgrade package and must never be picked by a plain driver name.
    Get-ChildItem $PackageRoot -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match "^$([regex]::Escape($Driver))-\d" } |
        Sort-Object { [version](($_.Name -split '-')[-1]) } -Descending |
        Select-Object -First 1
}

# ---------------------------------------------------------------- preflight
Set-Content -Path $log -Value "Install-SwitchDrivers $((Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))" -Encoding utf8
$IsAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
Say "package root : $PackageRoot"
Say "elevated     : $IsAdmin"
if (-not $IsAdmin -and -not $DryRun) {
    Write-Host ''
    Write-Host 'This must run elevated (Administrator). Right-click PowerShell -> Run as administrator.' -ForegroundColor Red
    Write-Host 'To preview what it would do, add -DryRun.' -ForegroundColor Yellow
    exit 1
}
if (-not $IsAdmin) { Say 'dry-run without elevation: nothing will be written' }
if (-not (Test-Path $PackageRoot)) {
    Write-Host "Package directory not found: $PackageRoot" -ForegroundColor Red
    exit 1
}

$Targets = if ($Only) { $Only } else { $Order }
if ($Only) {
    $missing = $Only | Where-Object { $Order -notcontains $_ }
    if ($missing) { Say "warning: not in the known install order: $($missing -join ', ')" }
}

if (-not (Test-Path (Join-Path $env:SystemRoot 'System32\drivers\ndis.sys'))) {
    Say 'warning: no ndis.sys here - this does not look like Windows'
}

# ---------------------------------------------------------------- certificates
if ($SkipCerts) {
    Say 'certificates  : skipped (-SkipCerts)'
} elseif (-not (Test-Path $CertDir)) {
    Say "certificates  : directory missing ($CertDir) - skipped"
} else {
    foreach ($cer in (Get-ChildItem $CertDir -Filter *.cer | Sort-Object Name)) {
        $cert = [Security.Cryptography.X509Certificates.X509Certificate2]::new($cer.FullName)
        Say ("certificate   : {0}  thumbprint={1}" -f $cer.Name, $cert.Thumbprint)
        foreach ($store in 'Root', 'TrustedPublisher') {
            $already = Get-ChildItem "Cert:\LocalMachine\$store" -ErrorAction SilentlyContinue |
                Where-Object { $_.Thumbprint -eq $cert.Thumbprint }
            if ($already) { Say ("                already trusted in LocalMachine\{0}" -f $store); continue }
            if ($DryRun) { Say ("                [dry-run] would import into LocalMachine\{0}" -f $store); continue }
            try {
                Import-Certificate -FilePath $cer.FullName -CertStoreLocation "Cert:\LocalMachine\$store" | Out-Null
                Say ("                imported into LocalMachine\{0}" -f $store)
            } catch {
                Say ("                FAILED to import into {0}: {1}" -f $store, $_.Exception.Message)
            }
        }
    }
}

$testSigning = (bcdedit /enum '{current}' 2>$null | Select-String -Pattern 'testsigning\s+Yes')
if ($testSigning) { Say 'testsigning  : on' }
else { Say 'testsigning  : OFF  -> these test-signed drivers will not load. Run: bcdedit /set testsigning on' }

# ---------------------------------------------------------------- install
$Failed = @()
foreach ($driver in $Targets) {
    $pkg = Find-Package $driver
    if (-not $pkg) { Say ("{0,-14} SKIP  no package under {1}" -f $driver, $PackageRoot); $Failed += "$driver (missing)"; continue }

    $inf = Get-ChildItem $pkg.FullName -Filter *.inf | Select-Object -First 1
    if (-not $inf) { Say ("{0,-14} SKIP  no .inf in {1}" -f $driver, $pkg.Name); $Failed += "$driver (no inf)"; continue }
    $ver = ((Select-String -Path $inf.FullName -Pattern 'DriverVer\s*=').Line -replace '.*DriverVer\s*=\s*', '').Split(',')[-1].Trim()

    if ($DryRun) {
        Say ("{0,-14} [dry-run] {1} ({2}) -> pnputil /add-driver `"{3}`" /install" -f $driver, $pkg.Name, $ver, $inf.FullName)
        continue
    }

    Say ("{0,-14} installing {1} ..." -f $driver, $pkg.Name)
    $out = & pnputil.exe /add-driver $inf.FullName /install 2>&1
    $code = $LASTEXITCODE
    $out | ForEach-Object { Add-Content -Path $log -Value ("    pnputil: " + $_) -Encoding utf8 }
    if ($code -eq 0) {
        Say ("{0,-14} ok (driver {1})" -f $driver, $ver)
    } else {
        Say ("{0,-14} FAILED (pnputil exit {1}) - see {2}" -f $driver, $code, $log)
        $Failed += "$driver (exit $code)"
    }
    Start-Sleep -Milliseconds 500
}

# ---------------------------------------------------------------- verify
Say ''
Say '=== verification ==='
$bound = @{}
try {
    Get-CimInstance Win32_PnPSignedDriver -ErrorAction Stop | ForEach-Object {
        $key = $_.InfName
        if ($key) { $bound[$key] = $_.DriverVersion }
    }
} catch { Say ("could not enumerate devices: " + $_.Exception.Message) }

$rows = @()
foreach ($driver in $Order + 'nvtsdhc') {
    if ($Expect.Keys -notcontains $driver) { continue }
    $pkg = Find-Package $driver
    $pkgVer = if ($pkg) {
        $i = Get-ChildItem $pkg.FullName -Filter *.inf | Select-Object -First 1
        if ($i) { ((Select-String -Path $i.FullName -Pattern 'DriverVer\s*=').Line -replace '.*DriverVer\s*=\s*', '').Split(',')[-1].Trim() } else { '(no inf)' }
    } else { '(no package)' }
    $rows += [pscustomobject]@{
        Driver   = $driver
        Expected = $Expect[$driver]
        InBundle = $pkgVer
        Running  = if ($bound[$driver + '.inf']) { $bound[$driver + '.inf'] } else { '-' }
    }
}
$rows | Format-Table -AutoSize | Out-String -Width 100 | Write-Host

if ($bound['switchwifi.inf']) {
    foreach ($line in (netsh wlan show interfaces)) {
        if ($line -match '^\s*(SSID|State)\s*:') { Say ("wlan: " + ($line -replace '\s+', ' ').Trim()) }
    }
}

Say ''
if ($Failed.Count) {
    Say ("RESULT: {0} problem(s): {1}" -f $Failed.Count, ($Failed -join '; '))
    Write-Host ''
    Write-Host 'Some steps failed. Read install.log, and see the troubleshooting section of INSTALL.md.' -ForegroundColor Yellow
    exit 1
}
Say 'RESULT: all requested drivers installed.'
Say ("log: " + $log)
