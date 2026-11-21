# sshd keeper for the WoA console.
#
# The real failure mode observed on 2026-11-21 is Wi-Fi link loss on the *client*
# side ("banner exchange" timeouts from the PC while the console itself answers
# instantly), plus occasional boots where the service is not running at all.
#
# Lesson learned the hard way: an earlier version restarted sshd whenever a local
# banner probe looked slow, which killed the very sessions that were working
# (a 3.6 MB benchmark saturating the CPU makes the probe time out). This version
# therefore NEVER restarts a running service:
#   - service not Running  -> Start-Service sshd
#   - service Running      -> only log the probe result, change nothing
param(
    [string]$Log = 'C:\SwitchWOA\sshd-keeper.log',
    [string]$HostAddress = ''
)

function Write-Log([string]$message) {
    ('{0}  {1}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $message) |
        Out-File -FilePath $Log -Append -Encoding utf8
}

$service = Get-Service sshd -ErrorAction SilentlyContinue
if (-not $service) { Write-Log 'sshd service missing'; exit 0 }
if ($service.Status -ne 'Running') {
    Write-Log ('service Stopped -> starting (was: ' + $service.Status + ')')
    try { Start-Service sshd; Start-Sleep -Seconds 2; Write-Log ('started: ' + (Get-Service sshd).Status) }
    catch { Write-Log ('start failed: ' + $_.Exception.Message) }
    exit 0
}

# Service is running: record a probe result for diagnostics only.
if (-not $HostAddress) {
    $HostAddress = (Get-NetIPAddress -AddressFamily IPv4 |
        Where-Object { $_.IPAddress -notlike '127.*' -and $_.IPAddress -notlike '169.254.*' } |
        Select-Object -First 1).IPAddress
}
if (-not $HostAddress) { exit 0 }

$banner = $false
try {
    $client = New-Object Net.Sockets.TcpClient
    $client.Connect($HostAddress, 22)
    $client.ReceiveTimeout = 5000
    $stream = $client.GetStream()
    $buffer = New-Object byte[] 64
    $deadline = (Get-Date).AddSeconds(5)
    while ((Get-Date) -lt $deadline) {
        if ($stream.DataAvailable) {
            if ($stream.Read($buffer, 0, 64) -gt 0) { $banner = $true }
            break
        }
        Start-Sleep -Milliseconds 150
    }
    $client.Close()
} catch {
    $banner = $false
}
if (-not $banner) { Write-Log ('note: local banner probe slow/failed on {0} (service kept running)' -f $HostAddress) }
exit 0
