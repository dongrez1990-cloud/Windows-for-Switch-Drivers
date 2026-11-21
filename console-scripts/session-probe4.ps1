# Interactive-session probe v4: what does a *normal* app pay to present a
# full-screen frame on this desktop?
#
# v3 established the real vsync interval: a 640x360 GDI window presents at
# 63.7 fps (15.71 ms/frame), so the panel is ~60 Hz class and DwmFlush cadence is
# a responsiveness metric, not the refresh rate. What is still unknown is the
# full-screen present cost - the number that decides whether any app can hold
# 60 fps here. This measures it with a real window (dirty-rect updates), at the
# desktop size, and compares window sizes to expose the per-pixel cost.
param(
    [string]$Out = 'C:\SwitchWOA\session-probe4.txt',
    [int]$Seconds = 3
)
$ErrorActionPreference = 'Continue'
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
"[probe4] $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') session=$((Get-Process -Id $PID).SessionId)" | Set-Content $Out

Add-Type -Namespace W -Name P4 -MemberDefinition @"
[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
[System.Runtime.InteropServices.DllImport("winmm.dll")] public static extern uint timeBeginPeriod(uint ms);
[System.Runtime.InteropServices.DllImport("winmm.dll")] public static extern uint timeEndPeriod(uint ms);
"@
$dw = [W.P4]::GetSystemMetrics(0); $dh = [W.P4]::GetSystemMetrics(1)
"desktop: {0}x{1}   timer resolution raised to 1 ms" -f $dw, $dh | Add-Content $Out
[void][W.P4]::timeBeginPeriod(1)

function Present-Test([int]$w, [int]$h, [string]$label) {
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.Clear([System.Drawing.Color]::DarkSlateGray)
    $g.FillRectangle([System.Drawing.Brushes]::Orange, 8, 8, [Math]::Max(16, $w / 8), [Math]::Max(16, $h / 8))
    $form = New-Object System.Windows.Forms.Form
    $form.FormBorderStyle = 'None'
    $form.StartPosition = 'Manual'
    $form.Location = New-Object System.Drawing.Point 0, 0
    $form.Size = New-Object System.Drawing.Size $w, $h
    $form.TopMost = $true
    $script:paints = 0
    $form.Add_Paint({
        param($s, $e)
        $e.Graphics.DrawImageUnscaled($bmp, 0, 0)
        $script:paints++
    })
    $form.Show()
    $null = $form.Handle   # force realisation before timing
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt ($Seconds * 1000)) {
        $form.Invalidate(); $form.Update()
        [System.Windows.Forms.Application]::DoEvents()
    }
    $sw.Stop()
    $frames = $script:paints
    $form.Close(); $form.Dispose(); $bmp.Dispose()
    $ms = $sw.Elapsed.TotalMilliseconds / [Math]::Max(1, $frames)
    $fps = $frames * 1000.0 / [Math]::Max(1, $sw.ElapsedMilliseconds)
    "{0} {1}x{2}: {3} frames / {4:N0} ms => {5:N2} ms/frame ({6:N1} fps)" -f `
        $label, $w, $h, $frames, $sw.Elapsed.TotalMilliseconds, $ms, $fps | Add-Content $Out
}

Present-Test 640 360 'small  '
Present-Test 1280 720 'desktop'
if ($dw -ne 1280 -or $dh -ne 720) { Present-Test $dw $dh 'desktop' }

[void][W.P4]::timeEndPeriod(1)
'probe4 done' | Add-Content $Out
