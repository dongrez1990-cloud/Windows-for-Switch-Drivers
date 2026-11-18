# Remote probe for switchwifi 0.7.3 rollout (runs on the Switch).
$ErrorActionPreference = 'Continue'
$id = [Security.Principal.WindowsIdentity]::GetCurrent()
"USER       = $($id.Name)"
"ELEVATED   = $((New-Object Security.Principal.WindowsPrincipal($id)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator))"
"GROUPS(adm)= " + ((whoami /groups) -join ' | ' -replace '\s+',' ')

"--- device ---"
try {
    $d = Get-PnpDevice -InstanceId 'ACPI\SWOA0D01\0' -ErrorAction Stop
    "STATUS     = $($d.Status) / $($d.ProblemCode)"
    "NAME       = $($d.FriendlyName)"
} catch { "PNP_ERR    = $($_.Exception.Message)" }

try {
    $v = (Get-PnpDeviceProperty -InstanceId 'ACPI\SWOA0D01\0' -KeyName DEVPKEY_Device_DriverVersion -ErrorAction Stop).Data
    "DRIVERVER  = $v"
} catch { "VER_ERR    = $($_.Exception.Message)" }

try {
    Get-NetAdapter -ErrorAction Stop | Select-Object Name,InterfaceDescription,Status,LinkSpeed |
        Format-Table -AutoSize | Out-String -Width 160
} catch { "NET_ERR    = $($_.Exception.Message)" }

"--- wireless ---"
try { (netsh wlan show interfaces) -join "`n" } catch { "NETSH_ERR  = $($_.Exception.Message)" }

"--- driver report (device hardware key) ---"
$k = 'HKLM:\SYSTEM\CurrentControlSet\Enum\ACPI\SWOA0D01\0'
try { "REPORT     = " + (Get-ItemProperty -Path $k -Name SwitchWifiReport -ErrorAction Stop).SwitchWifiReport }
catch { "REPORT_ERR = $($_.Exception.Message)" }

"--- driver packages ---"
try { (Get-WindowsDriver -Online -ErrorAction Stop | Where-Object { $_.Driver -match 'switchwifi' } | Select-Object Driver,Version,Date,OriginalFileName | Format-Table -AutoSize | Out-String -Width 160) }
catch { "WD_ERR     = $($_.Exception.Message)" }

"--- service ---"
try { Get-Service switchwifi -ErrorAction Stop | Select-Object Name,Status,StartType | Format-Table -AutoSize | Out-String } catch { "SVC_ERR    = $($_.Exception.Message)" }

"--- sshd config (how elevation could work) ---"
try { (Get-Content "$env:ProgramData\ssh\sshd_config" -ErrorAction Stop | Where-Object { $_ -match 'DefaultShell|Subsystem|ForceCommand' }) -join "`n" } catch { "SSHD_ERR   = $($_.Exception.Message)" }
try { (Get-CimInstance Win32_Service -Filter "Name='sshd'" -ErrorAction Stop | Select-Object Name,StartName,State | Format-List | Out-String) } catch { "SSHD_SVC_ERR = $($_.Exception.Message)" }

"--- scheduled tasks that look like helpers ---"
try { (Get-ScheduledTask -ErrorAction Stop | Where-Object { $_.TaskName -match 'Dsh|Switch|sshd' } | Select-Object TaskName,State | Format-Table -AutoSize | Out-String) } catch { "TASK_ERR   = $($_.Exception.Message)" }
