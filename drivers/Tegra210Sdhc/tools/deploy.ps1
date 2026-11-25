# Runs as SYSTEM: swap in the staged nvtsdhc.sys and clear the Diag key.
# Binding (Enum) is left untouched - only the driver file changes.
# Log: C:\Windows\Temp\stage1\deploy.txt
$ErrorActionPreference = 'Continue'
$dir = 'C:\Windows\Temp\stage1'
$out = Join-Path $dir 'deploy.txt'
$log = New-Object System.Collections.Generic.List[string]
$stamp = Get-Date -Format 'HHmmss'
$dst = 'C:\Windows\System32\drivers\nvtsdhc.sys'
$src = Join-Path $dir 'nvtsdhc.sys'

$log.Add("deploy started $(Get-Date -Format o) as $([Security.Principal.WindowsIdentity]::GetCurrent().Name)")

if (-not (Test-Path $src)) { $log.Add("STAGED FILE MISSING: $src"); $log -join "`n" | Set-Content $out -Encoding UTF8; exit 1 }

if (Test-Path $dst) {
    Copy-Item $dst "$dst.old-$stamp" -Force
    $log.Add("backup: nvtsdhc.sys -> nvtsdhc.sys.old-$stamp")
}
Copy-Item $src $dst -Force
$i = Get-Item $dst
$log.Add("installed: $($i.Length) bytes sha $((Get-FileHash $dst -Algorithm SHA256).Hash.Substring(0,8))")

$enum = 'HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\NVDA0212\0'
$log.Add('enum (unchanged): ' + ((reg query $enum 2>&1 | Select-String -Pattern 'Service|Driver') -join ' | '))
$svc = 'HKLM\SYSTEM\CurrentControlSet\Services\nvtsdhc'
$log.Add('service Start=' + ((reg query $svc /v Start 2>&1) -join ' '))

$diag = 'HKLM\SYSTEM\CurrentControlSet\Services\nvtsdhc\Parameters\Diag'
reg delete $diag /f 2>&1 | Out-Null
reg add $diag /f 2>&1 | Out-Null
$log.Add('diag cleared')

$log -join "`n" | Set-Content $out -Encoding UTF8
