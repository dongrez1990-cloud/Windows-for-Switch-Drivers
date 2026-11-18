# pack-029-as-036.ps1 — repackage the 0.2.9 binary unchanged under version 0.3.6.0.
# pnputil only upgrades, so the only way "back to 0.2.9" works is to give the exact
# same .sys a higher DriverVer and a freshly signed catalog.
param(
    [string]$Src9 = 'D:\paopao\switchjoycon\backup\0.2.9',
    [string]$Out  = 'D:\paopao\switchjoycon\out-029-036',
    [string]$NewVersion = '0.3.6.0'
)
$ErrorActionPreference = 'Stop'
$Kits = 'C:\Program Files (x86)\Windows Kits\10'
$SignTool = "$Kits\bin\10.0.26100.0\x64\signtool.exe"
$Inf2Cat  = "$Kits\bin\10.0.26100.0\x86\Inf2Cat.exe"
$CertSubject = 'CN=SwitchWOA Test Driver Signing'

New-Item -ItemType Directory -Force $Out | Out-Null
Get-ChildItem $Out -File | Remove-Item -Force -ErrorAction SilentlyContinue

# 1. the untouched 0.2.9 binary
Copy-Item (Join-Path $Src9 'switchjoycon.sys') (Join-Path $Out 'switchjoycon.sys') -Force
$hash = (Get-FileHash (Join-Path $Out 'switchjoycon.sys') -Algorithm SHA256).Hash
"sys hash : $hash"
if (-not $hash.StartsWith('7EE996F830A5D925')) { throw "0.2.9 sys hash mismatch: $hash" }

# 2. same INF, only DriverVer bumped
$inf = Get-Content -LiteralPath (Join-Path $Src9 'switchjoycon.inf') -Raw
$inf = [regex]::Replace($inf, 'DriverVer=\d+/\d+/\d+,[\d\.]+', "DriverVer=11/18/2026,$NewVersion")
[IO.File]::WriteAllText((Join-Path $Out 'switchjoycon.inf'), $inf, (New-Object System.Text.UTF8Encoding($false)))
"inf ver  : " + ([regex]::Match((Get-Content -Raw (Join-Path $Out 'switchjoycon.inf')), 'DriverVer=.*').Value)

# 3. sign the (already 0.2.9-signed) binary again so the catalog and file agree, then make+sign the catalog
$cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq $CertSubject -and $_.NotAfter -gt (Get-Date) } | Select-Object -First 1
if (-not $cert) { throw "test certificate not found in Cert:\CurrentUser\My" }
"cert     : $($cert.Thumbprint)"

& $SignTool sign /q /fd sha256 /sha1 $cert.Thumbprint /s My (Join-Path $Out 'switchjoycon.sys')
if ($LASTEXITCODE -ne 0) { throw 'signtool failed (sys)' }

& $Inf2Cat "/driver:$Out" '/os:10_VB_ARM64' '/uselocaltime' | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'Inf2Cat failed' }

Get-ChildItem $Out -Filter *.cat | ForEach-Object {
    & $SignTool sign /q /fd sha256 /sha1 $cert.Thumbprint /s My $_.FullName
    if ($LASTEXITCODE -ne 0) { throw "signtool failed ($($_.Name))" }
}

"--- result ---"
Get-ChildItem $Out | ForEach-Object {
    "  {0,7}  {1}  {2}" -f $_.Length, $_.Name, (Get-FileHash $_.FullName -Algorithm SHA256).Hash.Substring(0, 16)
}
& $SignTool verify /pa /q (Join-Path $Out 'switchjoycon.sys')
"signature: " + $(if ($LASTEXITCODE -eq 0) { 'ok' } else { 'FAILED' })
