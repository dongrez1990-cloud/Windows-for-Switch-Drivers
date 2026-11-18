$ProgressPreference = 'SilentlyContinue'
$ErrorActionPreference = 'Continue'
"== volumes =="
Get-Volume | Select-Object DriveLetter,FileSystemLabel,FileSystem,Size,SizeRemaining |
    Format-Table -AutoSize | Out-String -Width 140
"== uefi / hekate related files on the card =="
foreach ($d in (Get-Volume | Where-Object { $_.DriveLetter -and $_.FileSystem -in @('FAT32','FAT') } | ForEach-Object { "$($_.DriveLetter):" })) {
    "  $d"
    Get-ChildItem "$d\" -ErrorAction SilentlyContinue | Select-Object Mode,Name | Format-Table -AutoSize | Out-String -Width 100
    Get-ChildItem "$d\switchroot" -ErrorAction SilentlyContinue | Select-Object Mode,Name | Format-Table -AutoSize | Out-String -Width 100
    Get-ChildItem "$d\bootloader\ini" -ErrorAction SilentlyContinue | Select-Object Name | Format-Table -AutoSize | Out-String -Width 100
}
"== ACPI devices =="
Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
    Where-Object { $_.InstanceId -like 'ACPI\*' } |
    Select-Object Status,FriendlyName,InstanceId | Sort-Object InstanceId |
    Format-Table -AutoSize | Out-String -Width 160
"== switchwifi / switchbt quick =="
Get-Service switchwifi,switchbt,switchfan -ErrorAction SilentlyContinue |
    Select-Object Status,Name | Format-Table -AutoSize | Out-String
