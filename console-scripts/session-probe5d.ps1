# Interactive-session probe 5d: is the per-pixel cost the display path, or my
# measurement API?
#
# probe5c measured a full-screen update with GDI+ (DrawImageUnscaled):
# 71.5 ns/pixel (~125 cycles/pixel at 1.75 GHz), which is far too slow for a
# blit. This repeats the same area sweep with the plain GDI path that real
# applications use - BitBlt from a DIB section, and SetDIBitsToDevice - so the
# two can be compared directly.
param([string]$Out = 'C:\SwitchWOA\session-probe5d.txt', [int]$Seconds = 2)
$ErrorActionPreference = 'Continue'
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
Add-Type -Namespace W -Name P5D -MemberDefinition @"
[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern System.IntPtr GetDC(System.IntPtr h);
[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern int ReleaseDC(System.IntPtr h, System.IntPtr dc);
[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
[System.Runtime.InteropServices.DllImport("gdi32.dll")] public static extern System.IntPtr CreateCompatibleDC(System.IntPtr dc);
[System.Runtime.InteropServices.DllImport("gdi32.dll")] public static extern System.IntPtr CreateDIBSection(System.IntPtr dc, ref BITMAPINFO bmi, uint usage, out System.IntPtr bits, System.IntPtr section, uint offset);
[System.Runtime.InteropServices.DllImport("gdi32.dll")] public static extern System.IntPtr SelectObject(System.IntPtr dc, System.IntPtr obj);
[System.Runtime.InteropServices.DllImport("gdi32.dll")] public static extern bool BitBlt(System.IntPtr dst, int x, int y, int w, int h, System.IntPtr src, int sx, int sy, uint rop);
[System.Runtime.InteropServices.DllImport("gdi32.dll")] public static extern int SetDIBitsToDevice(System.IntPtr dc, int x, int y, uint w, uint h, int xs, int ys, uint start, uint lines, System.IntPtr bits, ref BITMAPINFO bmi, uint usage);
[System.Runtime.InteropServices.DllImport("gdi32.dll")] public static extern bool DeleteDC(System.IntPtr dc);
[System.Runtime.InteropServices.DllImport("gdi32.dll")] public static extern bool DeleteObject(System.IntPtr obj);
[System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential)]
public struct BITMAPINFOHEADER {
  public uint biSize; public int biWidth; public int biHeight; public ushort biPlanes; public ushort biBitCount;
  public uint biCompression; public uint biSizeImage; public int biXPelsPerMeter; public int biYPelsPerMeter;
  public uint biClrUsed; public uint biClrImportant;
}
[System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential)]
public struct BITMAPINFO { public BITMAPINFOHEADER bmiHeader; public uint bmiColors; }
"@

$SRCCOPY = 0x00CC0020
"[probe5d] $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') session=$((Get-Process -Id $PID).SessionId)" | Set-Content $Out
"desktop: {0}x{1}" -f [W.P5D]::GetSystemMetrics(0), [W.P5D]::GetSystemMetrics(1) | Add-Content $Out

# A plain window to present into.
$form = New-Object System.Windows.Forms.Form
$form.FormBorderStyle = 'None'
$form.StartPosition = 'Manual'
$form.Location = New-Object System.Drawing.Point 0, 0
$form.Size = New-Object System.Drawing.Size 1280, 720
$form.TopMost = $true
$form.BackColor = [System.Drawing.Color]::Black
$form.Show()
$null = $form.Handle
Start-Sleep -Milliseconds 500

function New-Surface([int]$w, [int]$h) {
    $bmi = New-Object W.P5D+BITMAPINFO
    $bmi.bmiHeader.biSize = 40
    $bmi.bmiHeader.biWidth = $w
    $bmi.bmiHeader.biHeight = -$h          # top-down
    $bmi.bmiHeader.biPlanes = 1
    $bmi.bmiHeader.biBitCount = 32
    $bmi.bmiHeader.biCompression = 0        # BI_RGB
    $bits = [IntPtr]::Zero
    $screenDc = [W.P5D]::GetDC([IntPtr]::Zero)
    $dib = [W.P5D]::CreateDIBSection($screenDc, [ref]$bmi, 0, [ref]$bits, [IntPtr]::Zero, 0)
    [void][W.P5D]::ReleaseDC([IntPtr]::Zero, $screenDc)
    $memDc = [W.P5D]::CreateCompatibleDC([IntPtr]::Zero)
    $old = [W.P5D]::SelectObject($memDc, $dib)
    # fill the DIB once
    $bytes = $w * $h * 4
    $managed = New-Object byte[] $bytes
    for ($i = 0; $i -lt $bytes; $i += 4) { $managed[$i] = 0x40; $managed[$i+1] = 0x80; $managed[$i+2] = 0xC0; $managed[$i+3] = 0xFF }
    [System.Runtime.InteropServices.Marshal]::Copy($managed, 0, $bits, $bytes)
    [pscustomobject]@{ bmi = $bmi; dib = $dib; memDc = $memDc; old = $old; bits = $bits; w = $w; h = $h }
}
function Free-Surface($s) {
    [void][W.P5D]::SelectObject($s.memDc, $s.old)
    [void][W.P5D]::DeleteObject($s.dib)
    [void][W.P5D]::DeleteDC($s.memDc)
}

$winDc = [W.P5D]::GetDC($form.Handle)
foreach ($size in @(@(640,360), @(960,540), @(1280,720))) {
    $w = $size[0]; $h = $size[1]
    $s = New-Surface $w $h
    $form.Size = New-Object System.Drawing.Size $w, $h
    Start-Sleep -Milliseconds 300

    # BitBlt from DIB -> window DC
    $null = [W.P5D]::BitBlt($winDc, 0, 0, $w, $h, $s.memDc, 0, 0, $SRCCOPY)
    $sw = [Diagnostics.Stopwatch]::StartNew(); $n = 0
    while ($sw.ElapsedMilliseconds -lt ($Seconds * 1000)) {
        if ([W.P5D]::BitBlt($winDc, 0, 0, $w, $h, $s.memDc, 0, 0, $SRCCOPY)) { $n++ }
        [System.Windows.Forms.Application]::DoEvents()
    }
    $sw.Stop()
    $ms = $sw.Elapsed.TotalMilliseconds / [Math]::Max(1, $n)
    'BitBlt      {0}x{1}: {2:N2} ms/frame ({3:N1} fps, {4:N1} ns/px)' -f `
        $w, $h, $ms, ($n * 1000.0 / $sw.ElapsedMilliseconds), ($ms * 1000000.0 / ($w * $h)) | Add-Content $Out

    # SetDIBitsToDevice straight into the window DC
    $null = [W.P5D]::SetDIBitsToDevice($winDc, 0, 0, $w, $h, 0, 0, 0, $h, $s.bits, [ref]$s.bmi, 0)
    $sw2 = [Diagnostics.Stopwatch]::StartNew(); $n2 = 0
    while ($sw2.ElapsedMilliseconds -lt ($Seconds * 1000)) {
        if ([W.P5D]::SetDIBitsToDevice($winDc, 0, 0, $w, $h, 0, 0, 0, $h, $s.bits, [ref]$s.bmi, 0) -ge 0) { $n2++ }
        [System.Windows.Forms.Application]::DoEvents()
    }
    $sw2.Stop()
    $ms2 = $sw2.Elapsed.TotalMilliseconds / [Math]::Max(1, $n2)
    'SetDIBits  {0}x{1}: {2:N2} ms/frame ({3:N1} fps, {4:N1} ns/px)' -f `
        $w, $h, $ms2, ($n2 * 1000.0 / $sw2.ElapsedMilliseconds), ($ms2 * 1000000.0 / ($w * $h)) | Add-Content $Out

    Free-Surface $s
}
[void][W.P5D]::ReleaseDC($form.Handle, $winDc)
$form.Close(); $form.Dispose()
'probe5d done' | Add-Content $Out
