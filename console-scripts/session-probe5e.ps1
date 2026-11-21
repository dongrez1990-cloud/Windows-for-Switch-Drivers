# Interactive-session probe 5e: sustained full-screen present rate.
#
# probe5d showed that enqueueing a full-screen frame with plain GDI BitBlt costs
# 0.17 ms regardless of size, and the 63-76 ms/frame numbers were GDI+
# DrawImageUnscaled overhead, not the display path. This measures what a real
# application sees: BitBlt a full frame, then DwmFlush to wait for the compositor,
# every frame. If the loop holds ~60 fps at 1280x720 the present path is not the
# bottleneck; the "20 fps" story must come from elsewhere.
param([string]$Out = 'C:\SwitchWOA\session-probe5e.txt', [int]$Seconds = 3)
$ErrorActionPreference = 'Continue'
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
Add-Type -Namespace W -Name P5E -MemberDefinition @"
[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern System.IntPtr GetDC(System.IntPtr h);
[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern int ReleaseDC(System.IntPtr h, System.IntPtr dc);
[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
[System.Runtime.InteropServices.DllImport("dwmapi.dll")] public static extern int DwmFlush();
[System.Runtime.InteropServices.DllImport("gdi32.dll")] public static extern System.IntPtr CreateCompatibleDC(System.IntPtr dc);
[System.Runtime.InteropServices.DllImport("gdi32.dll")] public static extern System.IntPtr CreateDIBSection(System.IntPtr dc, ref BITMAPINFO bmi, uint usage, out System.IntPtr bits, System.IntPtr section, uint offset);
[System.Runtime.InteropServices.DllImport("gdi32.dll")] public static extern System.IntPtr SelectObject(System.IntPtr dc, System.IntPtr obj);
[System.Runtime.InteropServices.DllImport("gdi32.dll")] public static extern bool BitBlt(System.IntPtr dst, int x, int y, int w, int h, System.IntPtr src, int sx, int sy, uint rop);
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
"[probe5e] $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') session=$((Get-Process -Id $PID).SessionId)" | Set-Content $Out

$form = New-Object System.Windows.Forms.Form
$form.FormBorderStyle = 'None'; $form.StartPosition = 'Manual'
$form.Location = New-Object System.Drawing.Point 0, 0
$form.Size = New-Object System.Drawing.Size 1280, 720
$form.TopMost = $true; $form.BackColor = [System.Drawing.Color]::Black
$form.Show(); $null = $form.Handle
Start-Sleep -Milliseconds 500
$winDc = [W.P5E]::GetDC($form.Handle)

foreach ($size in @(@(640,360), @(1280,720))) {
    $w = $size[0]; $h = $size[1]
    $bmi = New-Object W.P5E+BITMAPINFO
    $bmi.bmiHeader.biSize = 40; $bmi.bmiHeader.biWidth = $w; $bmi.bmiHeader.biHeight = -$h
    $bmi.bmiHeader.biPlanes = 1; $bmi.bmiHeader.biBitCount = 32; $bmi.bmiHeader.biCompression = 0
    $bits = [IntPtr]::Zero
    $dib = [W.P5E]::CreateDIBSection($winDc, [ref]$bmi, 0, [ref]$bits, [IntPtr]::Zero, 0)
    $memDc = [W.P5E]::CreateCompatibleDC($winDc)
    $old = [W.P5E]::SelectObject($memDc, $dib)
    $bytes = $w * $h * 4
    $managed = New-Object byte[] $bytes
    for ($i = 0; $i -lt $bytes; $i += 4) { $managed[$i] = 0x60; $managed[$i+1] = 0x20; $managed[$i+2] = 0xA0; $managed[$i+3] = 0xFF }
    [System.Runtime.InteropServices.Marshal]::Copy($managed, 0, $bits, $bytes)
    $form.Size = New-Object System.Drawing.Size $w, $h
    Start-Sleep -Milliseconds 300

    # enqueue only
    $sw = [Diagnostics.Stopwatch]::StartNew(); $n = 0
    while ($sw.ElapsedMilliseconds -lt ($Seconds * 1000)) { if ([W.P5E]::BitBlt($winDc,0,0,$w,$h,$memDc,0,0,$SRCCOPY)) { $n++ }; [System.Windows.Forms.Application]::DoEvents() }
    $sw.Stop()
    'BitBlt enqueue-only {0}x{1}: {2:N2} ms/frame ({3:N1} fps)' -f $w, $h, ($sw.Elapsed.TotalMilliseconds/[Math]::Max(1,$n)), ($n*1000.0/$sw.ElapsedMilliseconds) | Add-Content $Out

    # enqueue + wait for the compositor (what an app that paces to vsync sees)
    $sw2 = [Diagnostics.Stopwatch]::StartNew(); $n2 = 0
    while ($sw2.ElapsedMilliseconds -lt ($Seconds * 1000)) {
        if ([W.P5E]::BitBlt($winDc,0,0,$w,$h,$memDc,0,0,$SRCCOPY)) { $n2++ }
        [void][W.P5E]::DwmFlush()
        [System.Windows.Forms.Application]::DoEvents()
    }
    $sw2.Stop()
    'BitBlt + DwmFlush {0}x{1}: {2:N2} ms/frame ({3:N1} fps)' -f $w, $h, ($sw2.Elapsed.TotalMilliseconds/[Math]::Max(1,$n2)), ($n2*1000.0/$sw2.ElapsedMilliseconds) | Add-Content $Out

    [void][W.P5E]::SelectObject($memDc, $old)
    [void][W.P5E]::DeleteObject($dib)
    [void][W.P5E]::DeleteDC($memDc)
}
[void][W.P5E]::ReleaseDC($form.Handle, $winDc)
$form.Close(); $form.Dispose()
'probe5e done' | Add-Content $Out
