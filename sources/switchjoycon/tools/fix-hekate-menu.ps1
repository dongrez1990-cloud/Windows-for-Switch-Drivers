$ProgressPreference = 'SilentlyContinue'
$ErrorActionPreference = 'Stop'

# Hekate lists bootloader\ini files in directory order (Nyx also sorts by name),
# so a newly created entry ends up at the bottom of a full screen.  Rewrite the
# folder in the order we want, preserving every entry's original text:
#   00-swoa-v44.ini   JoyCon rail firmware  <== first / visible
#   01-swoa-v42.ini   the stable fallback (old 00-swoa-v42.ini text)
#   02-, 03-, ...     everything else, unchanged

$p = Get-Partition -DiskNumber 0 -PartitionNumber 1
if (-not $p.DriveLetter -or $p.DriveLetter -eq [char]0) {
    Set-Partition -DiskNumber 0 -PartitionNumber 1 -NewDriveLetter V
    Start-Sleep -Seconds 1
}
$dir = 'V:\bootloader\ini'
"== before =="
Get-ChildItem $dir -File | Select-Object Name, Length | Format-Table -AutoSize | Out-String -Width 100

# Snapshot every entry's text first.
$snap = @{}
foreach ($f in Get-ChildItem $dir -File) {
    $snap[$f.Name] = ($(Get-Content $f.FullName -Raw) -replace "`r`n", "`n")
}
"snapshot: " + (($snap.Keys | Sort-Object) -join ', ')

$v44Text = @"
[SWOA v44 JoyCon rail <= 1st]
l4t=1
boot_prefixes=switchroot/winx-v44/
id=WINXV44
cpu_oc=1785600
"@
$v44Text = ($v44Text -replace "`r`n", "`n" -replace "`n", "`r`n")

# Ordered plan: name -> text.  v42's text comes from the old 00- entry if present.
$plan = [ordered]@{}
$plan['00-swoa-v44.ini'] = $v44Text
if ($snap.ContainsKey('00-swoa-v42.ini')) { $plan['01-swoa-v42.ini'] = $snap['00-swoa-v42.ini'] }
$n = 2
foreach ($name in ($snap.Keys | Sort-Object)) {
    if ($name -in @('00-swoa-v42.ini', '48-swoa-v44.ini')) { continue }
    $plan[('{0:D2}-{1}' -f $n, ($name -replace '^\d\d-', ''))] = $snap[$name]
    $n++
}

# Delete (snapshot is in memory) then write in order.
foreach ($f in Get-ChildItem $dir -File) { Remove-Item $f.FullName -Force }
$enc = New-Object System.Text.ASCIIEncoding
foreach ($k in $plan.Keys) {
    [IO.File]::WriteAllText((Join-Path $dir $k), $plan[$k], $enc)
}

"== after =="
$after = Get-ChildItem $dir -File | Sort-Object Name
$after | Select-Object Name, Length | Format-Table -AutoSize | Out-String -Width 100
foreach ($f in $after) {
    "--- $($f.Name) ---"
    (Get-Content $f.FullName -Raw).Trim()
}
