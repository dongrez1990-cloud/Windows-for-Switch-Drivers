# Runs as SYSTEM right after boot: dump the hung boot's Diag evidence, then put
# the known-good nvtsdhc.sys (blk128, 32680 B / 41EFF78A) back in place so the
# next boot does not run the DMA probe again.
$ErrorActionPreference = 'Continue'
$dir = 'C:\Windows\Temp\stage1'
$out = Join-Path $dir 'rescue.txt'
$drvDir = 'C:\Windows\System32\drivers'
$dst = Join-Path $drvDir 'nvtsdhc.sys'
$log = New-Object System.Collections.Generic.List[string]

$log.Add("rescue started $(Get-Date -Format o)")

# 1) Evidence from the hung boot first.
$diagOut = Join-Path $dir 'diag-hang.txt'
$key = 'HKLM\SYSTEM\CurrentControlSet\Services\nvtsdhc\Parameters\Diag'
$q = reg query $key 2>&1 | ForEach-Object { $_.ToString() }
$q -join "`n" | Set-Content $diagOut -Encoding UTF8
$log.Add("diag captured -> $diagOut ($($q.Count) lines)")
foreach ($line in $q) {
    if ($line -match 'UhsDma|UhsSmmu|DataCommands|ClockKhz|BusSpeed|CommandErrors') { $log.Add('  ' + $line.Trim()) }
}

# 2) Prefer the explicitly staged known-good file, else any 32680-byte backup.
$staged = Join-Path $dir 'nvtsdhc.good.sys'
if (Test-Path $staged) {
    Copy-Item $dst "$dst.hungprobe-$((Get-Date).ToString('HHmmss'))" -Force -ErrorAction SilentlyContinue
    Copy-Item $staged $dst -Force
    $i = Get-Item $dst
    $log.Add("restored from staged nvtsdhc.good.sys: $($i.Length) bytes sha $((Get-FileHash $dst -Algorithm SHA256).Hash.Substring(0,8))")
    $log.Add('expect 32680 bytes sha 41EFF78A')
    $log -join "`n" | Set-Content $out -Encoding UTF8
    return
}

$cands = Get-ChildItem $drvDir -Filter 'nvtsdhc.sys.old-*' -ErrorAction SilentlyContinue |
         Sort-Object LastWriteTime -Descending
$log.Add("candidates: " + (($cands | ForEach-Object { "$($_.Name)=$($_.Length)" }) -join ', '))
$pick = $cands | Where-Object { $_.Length -eq 32680 } | Select-Object -First 1
if (-not $pick) { $pick = $cands | Select-Object -First 1 }
if ($pick) {
    Copy-Item $dst "$dst.hungprobe-$((Get-Date).ToString('HHmmss'))" -Force -ErrorAction SilentlyContinue
    Copy-Item $pick.FullName $dst -Force
    $i = Get-Item $dst
    $log.Add("restored from $($pick.Name): $($i.Length) bytes sha $((Get-FileHash $dst -Algorithm SHA256).Hash.Substring(0,8))")
    $log.Add('expect 32680 bytes sha 41EFF78A')
} else {
    $log.Add('NO BACKUP FOUND - left as is')
}

$log -join "`n" | Set-Content $out -Encoding UTF8
