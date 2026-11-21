# Interactive-session probe v3: is the ~46 fps cadence vsync-locked (hardware) or
# CPU-limited (software)? Plus the OS's own idea of the mode, and a real GDI
# window present rate.
#
# v2 result: DwmFlush paces at 21.78 ms/frame (45.9 fps) with DWM/System CPU at 0%
# on an idle desktop. Two hypotheses remain:
#   H1 hardware: the panel's scan rate really is ~46 Hz (hekate display timing),
#   H2 software: composition can only hit 46 fps at this CPU cost.
# Under a full CPU load H1 keeps the cadence, H2 degrades it.
$ErrorActionPreference = 'Continue'
$out = 'C:\SwitchWOA\session-probe3.txt'
"[probe3] $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') session=$((Get-Process -Id $PID).SessionId)" | Set-Content $out

Add-Type -AssemblyName System.Windows.Forms, System.Drawing
Add-Type -Namespace W -Name P3 -MemberDefinition @"
[System.Runtime.InteropServices.DllImport("dwmapi.dll")] public static extern int DwmFlush();
[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
[System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential, CharSet=System.Runtime.InteropServices.CharSet.Unicode)]
public struct DEVMODE {
  [System.Runtime.InteropServices.MarshalAs(System.Runtime.InteropServices.UnmanagedType.ByValTStr, SizeConst=32)] public string dmDeviceName;
  public ushort dmSpecVersion; public ushort dmDriverVersion; public ushort dmSize; public ushort dmDriverExtra;
  public uint dmFields; public int dmPositionX; public int dmPositionY; public uint dmDisplayOrientation; public uint dmDisplayFixedOutput;
  public short dmColor; public short dmDuplex; public short dmYResolution; public short dmTTOption; public short dmCollate;
  [System.Runtime.InteropServices.MarshalAs(System.Runtime.InteropServices.UnmanagedType.ByValTStr, SizeConst=32)] public string dmFormName;
  public ushort dmLogPixels; public uint dmBitsPerPel; public uint dmPelsWidth; public uint dmPelsHeight;
  public uint dmDisplayFlags; public uint dmDisplayFrequency;
  public uint dmICMMethod; public uint dmICMIntent; public uint dmMediaType; public uint dmDitherType;
  public uint dmReserved1; public uint dmReserved2; public uint dmPanningWidth; public uint dmPanningHeight;
}
[System.Runtime.InteropServices.DllImport("user32.dll", CharSet=System.Runtime.InteropServices.CharSet.Unicode)]
public static extern bool EnumDisplaySettings(string deviceName, int modeNum, ref DEVMODE devMode);
"@

# 1) What does the OS itself say the current mode is?
$dm = New-Object W.P3+DEVMODE
$dm.dmSize = [uint16][System.Runtime.InteropServices.Marshal]::SizeOf($dm)
if ([W.P3]::EnumDisplaySettings($null, -1, [ref]$dm)) {
    "DEVMODE: {0}x{1} {2}bpp {3}Hz orientation={4}" -f `
        $dm.dmPelsWidth, $dm.dmPelsHeight, $dm.dmBitsPerPel, $dm.dmDisplayFrequency, $dm.dmDisplayOrientation | Add-Content $out
} else { 'EnumDisplaySettings(-1) failed' | Add-Content $out }
$vc = Get-CimInstance Win32_VideoController -ErrorAction SilentlyContinue | Select-Object -First 1
if ($vc) { "WMI: {0} | mode={1} | refresh={2} | driver={3}" -f $vc.Name, $vc.VideoModeDescription, $vc.CurrentRefreshRate, $vc.DriverVersion | Add-Content $out }
else { 'WMI: no Win32_VideoController row' | Add-Content $out }

function Measure-Flush([int]$ms) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $n = 0
    while ($sw.ElapsedMilliseconds -lt $ms) { [void][W.P3]::DwmFlush(); $n++ }
    $sw.Stop()
    [pscustomobject]@{ frames = $n; ms = $sw.Elapsed.TotalMilliseconds }
}

$idle = Measure-Flush 4000
"idle   : {0} frames / {1:N0} ms => {2:N2} ms/frame ({3:N1} fps)" -f $idle.frames, $idle.ms, ($idle.ms / $idle.frames), ($idle.frames * 1000.0 / $idle.ms) | Add-Content $out

# 2) Same measurement with all four cores busy.
$jobs = 1..3 | ForEach-Object {
    Start-Job -ScriptBlock { $x = 1.0; $sw = [Diagnostics.Stopwatch]::StartNew(); while ($sw.ElapsedMilliseconds -lt 8000) { for ($i = 0; $i -lt 200000; $i++) { $x = $x * 1.0000001 } } }
}
Start-Sleep -Seconds 1
$load = Measure-Flush 4000
"loaded : {0} frames / {1:N0} ms => {2:N2} ms/frame ({3:N1} fps)" -f $load.frames, $load.ms, ($load.ms / $load.frames), ($load.frames * 1000.0 / $load.ms) | Add-Content $out
$jobs | ForEach-Object { Stop-Job $_ -ErrorAction SilentlyContinue; Remove-Job $_ -Force -ErrorAction SilentlyContinue }

# 3) A real GDI window present rate (dirty-rect updates, not full-screen DC writes).
$w = 640; $h = 360
$form = New-Object System.Windows.Forms.Form
$form.FormBorderStyle = 'None'
$form.StartPosition = 'CenterScreen'
$form.Size = New-Object System.Drawing.Size $w, $h
$form.TopMost = $true
$script:paints = 0
$script:stamp = 0
$bitmap = New-Object System.Drawing.Bitmap $w, $h
$bg = [System.Drawing.Graphics]::FromImage($bitmap)
$bg.Clear([System.Drawing.Color]::DarkGreen)
$bg.FillRectangle([System.Drawing.Brushes]::White, 10, 10, 100, 40)
$form.Add_Paint({
    param($s, $e)
    $e.Graphics.DrawImageUnscaled($bitmap, 0, 0)
    $script:paints++
})
$form.Show()
$sw3 = [Diagnostics.Stopwatch]::StartNew()
while ($sw3.ElapsedMilliseconds -lt 3000) {
    $form.Invalidate(); $form.Update()
    [System.Windows.Forms.Application]::DoEvents()
}
$sw3.Stop()
$paints = $script:paints
$form.Close(); $form.Dispose()
"window {0}x{1} GDI present: {2} frames / {3:N0} ms => {4:N2} ms/frame ({5:N1} fps)" -f `
    $w, $h, $paints, $sw3.Elapsed.TotalMilliseconds, ($sw3.Elapsed.TotalMilliseconds / [Math]::Max(1, $paints)), ($paints * 1000.0 / $sw3.ElapsedMilliseconds) | Add-Content $out
'probe3 done' | Add-Content $out
