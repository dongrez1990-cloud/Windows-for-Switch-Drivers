# Bring-up stability soak: samples the GPU driver state and the fault logs for N
# minutes and appends one line per sample to C:\SwitchWOA\soak.log.
#
# Goal: prove (or disprove) that the current bring-up settings stay healthy while
# the console runs, and catch the two failure classes seen before - WHEA
# (bus/power) and DPC_WATCHDOG/bugcheck (submission stall) - without touching the
# driver. Read-only apart from writing the log.
param(
    [string]$Log = 'C:\SwitchWOA\soak.log',
    [int]$Minutes = 40,
    [int]$IntervalSeconds = 30
)
$ErrorActionPreference = 'Continue'
$params = 'HKLM:\SYSTEM\CurrentControlSet\Services\tegragpurender\Parameters'
$deadline = (Get-Date).AddMinutes($Minutes)

"=== soak start $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') for ${Minutes} min ===" | Add-Content -Path $Log
while ((Get-Date) -lt $deadline) {
    $p = Get-ItemProperty $params -ErrorAction SilentlyContinue
    $dev = Get-PnpDevice -InstanceId 'ACPI\SWOA0F01\0' -ErrorAction SilentlyContinue
    $problem = try { (Get-PnpDeviceProperty -InstanceId 'ACPI\SWOA0F01\0' -KeyName 'DEVPKEY_Device_ProblemCode' -ErrorAction Stop).Data } catch { 'n/a' }
    $status = if ($dev) { $dev.Status } else { 'missing' }
    $whea = (Get-WinEvent -FilterHashtable @{LogName='System'; Id=17,18,19,20,41,1001,117} -MaxEvents 60 -ErrorAction SilentlyContinue |
             Where-Object { $_.TimeCreated -gt (Get-Date).AddMinutes(-$Minutes) -and
                            $_.ProviderName -match 'WHEA|BugCheck|Kernel-Power|SystemErrorReporting' }).Count
    $uptime = [int64]((Get-Date) - (Get-CimInstance Win32_OperatingSystem).LastBootUpTime).TotalSeconds
    $mem = [int64]((Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory) / 1024
    ('{0} up={1}s dev={2}/problem={3} hw=0x{4:X} clock={5} khz={6} step={7} gr={8} worker={9} freeMB={10:N0} faults={11}' -f `
        (Get-Date -Format 'HH:mm:ss'), $uptime, $status, $problem,
        [uint32]$p.HardwareStatus, $p.ClockResult, $p.BootGpuMeasuredKhz, $p.Step, $p.GrStage, $p.StatusWorker, $mem, $whea) |
        Add-Content -Path $Log
    Start-Sleep -Seconds $IntervalSeconds
}
"=== soak end $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') ===" | Add-Content -Path $Log
