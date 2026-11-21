# Interactive-session (session 1) display probe v2: who burns the CPU while the
# desktop composes?
#
# v1 found: real desktop = 1280x720 logical / 720x1280 signal / 64 Hz /
# composition ON, and DwmFlush paces at ~22 ms (45 fps) on an idle desktop.
# If DWM is pinning a core during that loop, the per-frame transform (the 90
# degree rotation into the portrait panel) is being done in software and is the
# pacing limit. This probe measures that without changing any setting.
$ErrorActionPreference = 'Continue'
$out = 'C:\SwitchWOA\session-probe2.txt'
"[probe2] $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') session=$((Get-Process -Id $PID).SessionId)" | Set-Content $out

Add-Type -Namespace W -Name P2 -MemberDefinition @"
[System.Runtime.InteropServices.DllImport("dwmapi.dll")] public static extern int DwmFlush();
[System.Runtime.InteropServices.DllImport("dwmapi.dll")] public static extern int DwmGetCompositionTimingInfo(System.IntPtr hwnd, [In, Out] ref DWM_TIMING_INFO info);
[System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential)]
public struct DWM_TIMING_INFO {
    public uint cbSize; public ulong qpcRefreshPeriod; public ulong qpcVBlank;
    public ulong cRefresh; public ulong cDXRefresh; public ulong qpcCompose;
    public ulong cFrame; public ulong cDXPresent; public ulong cRefreshFrame;
    public ulong cFrameSubmitted; public ulong cDXPresentSubmitted; public ulong cFrameConfirmed;
    public ulong cDXPresentConfirmed; public ulong cRefreshConfirmed; public ulong cDXRefreshConfirmed;
    public ulong cFramesLate; public ulong cFramesOutstanding; public ulong cFrameDisplayed;
    public ulong qpcFrameDisplayed; public ulong cRefreshFrameDisplayed; public ulong cFrameComplete;
    public ulong qpcFrameComplete; public ulong cFramePending; public ulong qpcFramePending;
    public ulong cFramesDisplayed; public ulong cFramesComplete; public ulong cFramesPending;
    public ulong cFramesAvailable; public ulong cFramesDropped; public ulong cFramesMissed;
    public ulong cRefreshNextDisplayed; public ulong cRefreshNextPresented; public ulong cRefreshesDisplayed;
    public ulong cRefreshesPresented; public ulong cRefreshStarted; public ulong cPixelsReceived;
    public ulong cPixelsDrawn; public ulong cBuffersEmpty;
}
"@

# 5 seconds of composition, sampling DWM / System / total CPU before and after.
function Cpu([string]$name) {
    $p = Get-Process -Name $name -ErrorAction SilentlyContinue
    if (-not $p) { return -1 }
    ($p | Measure-Object -Property CPU -Sum).Sum
}

$dwm0 = Cpu 'dwm'; $sys0 = Cpu 'System'
$sw = [Diagnostics.Stopwatch]::StartNew()
$frames = 0
while ($sw.ElapsedMilliseconds -lt 5000) { [void][W.P2]::DwmFlush(); $frames++ }
$sw.Stop()
$dwm1 = Cpu 'dwm'; $sys1 = Cpu 'System'

$secs = $sw.Elapsed.TotalSeconds
"loop: {0} flushes in {1:N2} s => {2:N2} ms/frame ({3:N1} fps)" -f `
    $frames, $secs, ($sw.Elapsed.TotalMilliseconds / $frames), ($frames / $secs) | Add-Content $out
"dwm cpu:     {0:N2} s over {1:N2} s => {2:N0}% of one core" -f ($dwm1 - $dwm0), $secs, (($dwm1 - $dwm0) * 100.0 / $secs) | Add-Content $out
"System cpu:  {0:N2} s over {1:N2} s => {2:N0}% of one core" -f ($sys1 - $sys0), $secs, (($sys1 - $sys0) * 100.0 / $secs) | Add-Content $out

# Compositor timing: refresh period, presented frames, dropped frames.
$ti = New-Object W.P2+DWM_TIMING_INFO
$ti.cbSize = [System.Runtime.InteropServices.Marshal]::SizeOf($ti)
if ([W.P2]::DwmGetCompositionTimingInfo([IntPtr]::Zero, [ref]$ti) -eq 0) {
    $refreshHz = [math]::Round(10000000.0 / $ti.qpcRefreshPeriod, 2)
    "timing: refresh_period={0} (=> {1} Hz) cRefresh={2} cFrame={3} cFrameDisplayed={4} cFramesLate={5} cFramesMissed={6} cBuffersEmpty={7}" -f `
        $ti.qpcRefreshPeriod, $refreshHz, $ti.cRefresh, $ti.cFrame, $ti.cFrameDisplayed, $ti.cFramesLate, $ti.cFramesMissed, $ti.cBuffersEmpty | Add-Content $out
    "timing: cPixelsDrawn={0} cPixelsReceived={1}" -f $ti.cPixelsDrawn, $ti.cPixelsReceived | Add-Content $out
} else {
    'DwmGetCompositionTimingInfo failed' | Add-Content $out
}

# Total system CPU over the same window, for context (4 cores).
$cpu = Get-CimInstance Win32_PerfFormattedData_PerfOS_Processor -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -eq '_Total' } | Select-Object -First 1
if ($cpu) { "system total cpu now: {0}%" -f $cpu.PercentProcessorTime | Add-Content $out }
'probe2 done' | Add-Content $out
