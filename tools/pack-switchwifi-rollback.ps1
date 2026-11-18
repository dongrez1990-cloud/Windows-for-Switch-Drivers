# pack-switchwifi-rollback.ps1 — repackage the currently installed 0.7.0.0 binary
# unchanged under a higher DriverVer so pnputil will accept it as an "upgrade".
# pnputil only upgrades: this is the only way back to 0.7.0.0 without a driver-store
# deletion (which is the same class of risk as uninstalling the adapter).
param(
    [string]$Src = 'D:\paopao\.tmp\wifi073\rollback-src',
    [string]$Out = 'D:\paopao\.tmp\wifi073\rollback-079',
    [string]$NewVersion = '0.7.9.0'
)
$ErrorActionPreference = 'Stop'
$Kits = 'C:\Program Files (x86)\Windows Kits\10'
$SignTool = "$Kits\bin\10.0.26100.0\x64\signtool.exe"
$Inf2Cat = "$Kits\bin\10.0.26100.0\x86\Inf2Cat.exe"
$CertSubject = 'CN=SwitchWOA Test Driver Signing'

New-Item -ItemType Directory -Force $Out | Out-Null
Get-ChildItem $Out -File | Remove-Item -Force -ErrorAction SilentlyContinue

# 1. the untouched 0.7.0.0 binary that is running right now
Copy-Item (Join-Path $Src 'switchwifi.sys') (Join-Path $Out 'switchwifi.sys') -Force
$hash = (Get-FileHash (Join-Path $Out 'switchwifi.sys') -Algorithm SHA256).Hash
"sys hash : $hash"
if (-not $hash.StartsWith('576096892EC5202C')) { throw "0.7.0.0 sys hash mismatch: $hash" }

# 2. same INF, only DriverVer bumped (0.7.0.0 -> $NewVersion)
$inf = Get-Content -LiteralPath (Join-Path $Src 'switchwifi.inf') -Raw
$inf = [regex]::Replace($inf, 'DriverVer=\d+/\d+/\d+,[\d\.]+', "DriverVer=11/18/2026,$NewVersion")
[IO.File]::WriteAllText((Join-Path $Out 'switchwifi.inf'), $inf, (New-Object System.Text.UTF8Encoding($false)))
"inf ver  : " + ([regex]::Match((Get-Content -Raw (Join-Path $Out 'switchwifi.inf')), 'DriverVer=.*').Value)

# 3. re-sign the binary and mint a fresh catalog
$cert = Get-ChildItem Cert:\CurrentUser\My |
    Where-Object { $_.Subject -eq $CertSubject -and $_.NotAfter -gt (Get-Date) } | Select-Object -First 1
if (-not $cert) { throw 'test certificate not found in Cert:\CurrentUser\My' }
"cert     : $($cert.Thumbprint)"

& $SignTool sign /q /fd sha256 /sha1 $cert.Thumbprint /s My (Join-Path $Out 'switchwifi.sys')
if ($LASTEXITCODE -ne 0) { throw 'signtool failed (sys)' }

& $Inf2Cat "/driver:$Out" '/os:10_NI_ARM64' '/uselocaltime' | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'Inf2Cat failed' }

Get-ChildItem $Out -Filter *.cat | ForEach-Object {
    & $SignTool sign /q /fd sha256 /sha1 $cert.Thumbprint /s My $_.FullName
    if ($LASTEXITCODE -ne 0) { throw "signtool failed ($($_.Name))" }
}

'--- result ---'
Get-ChildItem $Out | ForEach-Object {
    '  {0,7}  {1}  {2}' -f $_.Length, $_.Name, (Get-FileHash $_.FullName -Algorithm SHA256).Hash.Substring(0, 16)
}
