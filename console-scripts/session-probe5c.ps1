# Interactive-session probe 5c: attribute the full-screen present cost.
#
# The panel exposes exactly one mode (1280x720; a 720x1280 request returns
# DISP_CHANGE_BADMODE), so the rotation A/B is done by changing the *orientation*
# on the same resolution:
#   A: 1280x720 orientation 90  (current: rotated into the portrait panel)
#   B: 1280x720 orientation 0   (no rotation requested; image appears sideways)
# Then an area sweep quantifies the per-pixel cost in the restored mode.
# A separate safety task re-applies orientation 90 after 200 s anyway.
param([string]$Out = 'C:\SwitchWOA\session-probe5c.txt', [int]$Seconds = 2)
$ErrorActionPreference = 'Continue'
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
Add-Type -Namespace W -Name P5C -MemberDefinition @"
[System.Runtime.InteropServices.DllImport("user32.dll", CharSet=System.Runtime.InteropServices.CharSet.Unicode)]
public static extern int ChangeDisplaySettingsEx(string deviceName, ref DEVMODE devMode, System.IntPtr hwnd, uint flags, System.IntPtr param);
[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
[System.Runtime.InteropServices.DllImport("dwmapi.dll")] public static extern int DwmFlush();
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
"@
$DM_ORIENTATION = 0x80; $DM_PELSWIDTH = 0x80000; $DM_PELSHEIGHT = 0x100000; $DM_BPP = 0x40000

function Set-Mode([uint32]$w, [uint32]$h, [uint32]$orient) {
    $dm = New-Object W.P5C+DEVMODE
    $dm.dmSize = [uint16]220
    $dm.dmPelsWidth = $w; $dm.dmPelsHeight = $h; $dm.dmBitsPerPel = 32
    $dm.dmDisplayOrientation = $orient
    $dm.dmFields = $DM_PELSWIDTH -bor $DM_PELSHEIGHT -bor $DM_ORIENTATION -bor $DM_BPP
    [W.P5C]::ChangeDisplaySettingsEx($null, [ref]$dm, [IntPtr]::Zero, 0, [IntPtr]::Zero)
}
function Metrics { '{0}x{1}' -f [W.P5C]::GetSystemMetrics(0), [W.P5C]::GetSystemMetrics(1) }

function Measure-Present([int]$w, [int]$h) {
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.Clear([System.Drawing.Color]::DarkSlateGray)
    $g.FillRectangle([System.Drawing.Brushes]::Orange, 8, 8, [Math]::Max(16, [int]($w / 8)), [Math]::Max(16, [int]($h / 8)))
    $form = New-Object System.Windows.Forms.Form
    $form.FormBorderStyle = 'None'; $form.StartPosition = 'Manual'
    $form.Location = New-Object System.Drawing.Point 0, 0
    $form.Size = New-Object System.Drawing.Size $w, $h
    $form.TopMost = $true
    $script:paints = 0
    $form.Add_Paint({ param($s, $e) $e.Graphics.DrawImageUnscaled($bmp, 0, 0); $script:paints++ })
    $form.Show(); $null = $form.Handle
    Start-Sleep -Milliseconds 400
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt ($Seconds * 1000)) {
        $form.Invalidate(); $form.Update()
        [System.Windows.Forms.Application]::DoEvents()
    }
    $sw.Stop()
    $frames = $script:paints
    $form.Close(); $form.Dispose(); $bmp.Dispose()
    [pscustomobject]@{
        ms = $sw.Elapsed.TotalMilliseconds / [Math]::Max(1, $frames)
        fps = $frames * 1000.0 / [Math]::Max(1, $sw.Elapsed.TotalMilliseconds)
    }
}

"[probe5c] $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') session=$((Get-Process -Id $PID).SessionId)" | Set-Content $Out
"start metrics: $(Metrics)" | Add-Content $Out
'1280 720 32 1' | Set-Content 'C:\SwitchWOA\display-saved.txt'

try {
    # A: current landscape (rotation 90)
    $a = Measure-Present 1280 720
    ("A orient=90 1280x720 : {0:N2} ms/frame ({1:N1} fps)" -f $a.ms, $a.fps) | Add-Content $Out

    # B: same resolution, no rotation
    $rc = Set-Mode 1280 720 0
    "ChangeDisplaySettingsEx(1280x720 orient=0) => $rc" | Add-Content $Out
    Start-Sleep -Milliseconds 2500
    "after orient=0 metrics: $(Metrics)" | Add-Content $Out
    if ($rc -eq 0) {
        $b = Measure-Present 1280 720
        ("B orient=0  1280x720 : {0:N2} ms/frame ({1:N1} fps)" -f $b.ms, $b.fps) | Add-Content $Out
    } else {
        'B skipped: orientation change refused' | Add-Content $Out
    }
} finally {
    $rrc = Set-Mode 1280 720 1
    "restore orient=90 => $rrc" | Add-Content $Out
    Start-Sleep -Milliseconds 2500
    "restored metrics: $(Metrics)" | Add-Content $Out
}

# Area sweep in the restored mode: per-pixel cost of a full-screen-sized update.
foreach ($size in @(@(640, 360), @(960, 540), @(1280, 720))) {
    $r = Measure-Present $size[0] $size[1]
    ("area {0}x{1} ({2:N2} Mpx): {3:N2} ms/frame ({4:N1} fps)" -f `
        $size[0], $size[1], ($size[0] * $size[1] / 1000000.0), $r.ms, $r.fps) | Add-Content $Out
}
'probe5c done' | Add-Content $Out
