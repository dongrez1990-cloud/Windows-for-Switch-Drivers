# Interactive-session probe 5b: rotation A/B without the enumeration APIs.
#
# EnumDisplayDevices/EnumDisplaySettings fail in this environment, so the target
# and restore DEVMODEs are built explicitly:
#   portrait target : 720x1280, orientation DMDO_DEFAULT (0)  -> no rotation at all
#   restore         : 1280x720, orientation DMDO_90 (1)       -> the saved landscape mode
# Measures a full-screen present in each configuration. A separate safety task
# (C:\SwitchWOA\display-safety.ps1) re-applies the landscape mode 200 s later in
# case this process dies mid-way.
param([string]$Out = 'C:\SwitchWOA\session-probe5b.txt', [int]$Seconds = 2)
$ErrorActionPreference = 'Continue'
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
Add-Type -Namespace W -Name P5B -MemberDefinition @"
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
    $dm = New-Object W.P5B+DEVMODE
    $dm.dmSize = [uint16]220
    $dm.dmPelsWidth = $w
    $dm.dmPelsHeight = $h
    $dm.dmBitsPerPel = 32
    $dm.dmDisplayOrientation = $orient
    $dm.dmFields = $DM_PELSWIDTH -bor $DM_PELSHEIGHT -bor $DM_ORIENTATION -bor $DM_BPP
    [W.P5B]::ChangeDisplaySettingsEx($null, [ref]$dm, [IntPtr]::Zero, 0, [IntPtr]::Zero)
}
function Metrics { '{0}x{1}' -f [W.P5B]::GetSystemMetrics(0), [W.P5B]::GetSystemMetrics(1) }

function Measure-Present([int]$w, [int]$h) {
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.Clear([System.Drawing.Color]::DarkSlateGray)
    $g.FillRectangle([System.Drawing.Brushes]::Orange, 8, 8, [Math]::Max(16, [int]($w / 8)), [Math]::Max(16, [int]($h / 8)))
    $form = New-Object System.Windows.Forms.Form
    $form.FormBorderStyle = 'None'
    $form.StartPosition = 'Manual'
    $form.Location = New-Object System.Drawing.Point 0, 0
    $form.Size = New-Object System.Drawing.Size $w, $h
    $form.TopMost = $true
    $script:paints = 0
    $form.Add_Paint({ param($s, $e) $e.Graphics.DrawImageUnscaled($bmp, 0, 0); $script:paints++ })
    $form.Show()
    $null = $form.Handle
    Start-Sleep -Milliseconds 500
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt ($Seconds * 1000)) {
        $form.Invalidate(); $form.Update()
        [System.Windows.Forms.Application]::DoEvents()
    }
    $sw.Stop()
    $frames = $script:paints
    $form.Close(); $form.Dispose(); $bmp.Dispose()
    '{0:N2} ms/frame ({1:N1} fps)' -f ($sw.Elapsed.TotalMilliseconds / [Math]::Max(1, $frames)), ($frames * 1000.0 / [Math]::Max(1, $sw.Elapsed.TotalMilliseconds))
}

"[probe5b] $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') session=$((Get-Process -Id $PID).SessionId)" | Set-Content $Out
"start metrics: $(Metrics)" | Add-Content $Out
'1280 720 32 1' | Set-Content 'C:\SwitchWOA\display-saved.txt'   # landscape restore recipe

try {
    $before = Measure-Present 1280 720
    "A landscape 1280x720 (rotation 90): $before" | Add-Content $Out

    $rc = Set-Mode 720 1280 0
    "ChangeDisplaySettingsEx(720x1280 orient=0) => $rc (0 = success)" | Add-Content $Out
    Start-Sleep -Milliseconds 2000
    "after switch metrics: $(Metrics)" | Add-Content $Out
    if ($rc -eq 0) {
        $port = Measure-Present 720 1280
        "B portrait 720x1280 (identity, no rotation): $port" | Add-Content $Out
    }
} finally {
    $rrc = Set-Mode 1280 720 1
    "restore(1280x720 orient=90) => $rrc" | Add-Content $Out
    Start-Sleep -Milliseconds 2000
    "final metrics: $(Metrics)" | Add-Content $Out
}
'probe5b done' | Add-Content $Out
