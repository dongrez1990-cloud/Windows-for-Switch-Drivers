# Probe the *interactive* console session (session 1), not the SSH service session.
#
# Why: SSH commands on Windows run as a service in session 0, which has its own
# synthetic 1024x768 display and no DWM. Everything measured there (screen caps,
# "composition disabled", "present costs 0.14 ms") describes session 0's virtual
# framebuffer, not the panel. This script is started by a scheduled task that runs
# in the logged-on user's session, so its numbers describe the real desktop.
$ErrorActionPreference = 'Continue'
$out = 'C:\SwitchWOA\session-probe.txt'
"[probe] $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')  session=$( (Get-Process -Id $PID).SessionId )  user=$(whoami)" | Set-Content $out

Add-Type -AssemblyName System.Drawing
Add-Type -Namespace W -Name P -MemberDefinition @"
[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern System.IntPtr GetDC(System.IntPtr h);
[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
[System.Runtime.InteropServices.DllImport("gdi32.dll")] public static extern int GetDeviceCaps(System.IntPtr dc, int i);
[System.Runtime.InteropServices.DllImport("dwmapi.dll")] public static extern int DwmFlush();
[System.Runtime.InteropServices.DllImport("dwmapi.dll")] public static extern int DwmIsCompositionEnabled(out bool enabled);
"@

$dc = [W.P]::GetDC([IntPtr]::Zero)
$horz = [W.P]::GetDeviceCaps($dc, 8); $vert = [W.P]::GetDeviceCaps($dc, 10)
$bpp = [W.P]::GetDeviceCaps($dc, 12); $hz = [W.P]::GetDeviceCaps($dc, 116)
$comp = $false
[void][W.P]::DwmIsCompositionEnabled([ref]$comp)
"screen: {0}x{1} {2}bpp {3}Hz   metrics={4}x{5}   composition={6}" -f `
    $horz, $vert, $bpp, $hz, [W.P]::GetSystemMetrics(0), [W.P]::GetSystemMetrics(1), $comp | Add-Content $out

# Composition cadence in the real session.
$n = 60
$sw = [Diagnostics.Stopwatch]::StartNew()
for ($i = 0; $i -lt $n; $i++) { [void][W.P]::DwmFlush() }
$sw.Stop()
$flushMs = $sw.Elapsed.TotalMilliseconds / $n
"DwmFlush x{0}: {1:N2} ms/frame => {2:N1} fps" -f $n, $flushMs, (1000.0 / $flushMs) | Add-Content $out

# Real fullscreen present cost: draw one desktop-sized frame N times.
$bmp = New-Object System.Drawing.Bitmap $horz, $vert
$bg = [System.Drawing.Graphics]::FromImage($bmp)
$bg.Clear([System.Drawing.Color]::MidnightBlue)
$bg.FillRectangle([System.Drawing.Brushes]::Gold, 40, 40, 200, 120)
$screen = [System.Drawing.Graphics]::FromHwnd([IntPtr]::Zero)
if (-not $screen) { 'ERROR: no screen DC' | Add-Content $out; exit 1 }
$null = $screen.DrawImageUnscaled($bmp, 0, 0)
$n2 = 40
$sw2 = [Diagnostics.Stopwatch]::StartNew()
for ($i = 0; $i -lt $n2; $i++) { $screen.DrawImageUnscaled($bmp, 0, 0) }
$sw2.Stop()
$drawMs = $sw2.Elapsed.TotalMilliseconds / $n2
"GDI+ fullscreen draw {0}x{1}: {2:N2} ms/frame => {3:N1} fps ceiling" -f $horz, $vert, $drawMs, (1000.0 / $drawMs) | Add-Content $out

# Draw + wait for composition (what a real frame costs end to end).
$sw3 = [Diagnostics.Stopwatch]::StartNew()
for ($i = 0; $i -lt 30; $i++) { $screen.DrawImageUnscaled($bmp, 0, 0); [void][W.P]::DwmFlush() }
$sw3.Stop()
"draw+DwmFlush x30: {0:N2} ms/frame => {1:N1} fps" -f ($sw3.Elapsed.TotalMilliseconds / 30), (30000.0 / $sw3.ElapsedMilliseconds) | Add-Content $out

# Ask the compositor what it thinks the monitor refresh and mode are.
$dwmKey = 'HKCU:\Control Panel\Desktop'
"MODE: " + ((Get-CimInstance -Namespace root\wmi -ClassName WmiMonitorListedSupportedSourceModes -ErrorAction SilentlyContinue | Select-Object -First 1 | Out-String -Width 200)) | Add-Content $out
'probe done' | Add-Content $out
