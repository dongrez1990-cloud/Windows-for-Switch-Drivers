# Pure sequential reads, no writes: repeated unbuffered passes over one file.
param([int]$Mb = 192, [int]$Passes = 3)
$ErrorActionPreference = 'Continue'
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.ComponentModel;
public static class RawReadOnly
{
    const uint GENERIC_READ = 0x80000000;
    const uint OPEN_EXISTING = 3;
    const uint NB = 0x20000000, SS = 0x08000000;
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern IntPtr CreateFileW(string n, uint a, uint s, IntPtr sec, uint d, uint f, IntPtr t);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool ReadFile(IntPtr h, IntPtr b, uint len, out uint r, IntPtr o);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool SetFilePointerEx(IntPtr h, long dist, out long np, uint method);
    public static string RunPass(string path, long total, int block)
    {
        IntPtr h = CreateFileW(path, GENERIC_READ, 1, IntPtr.Zero, OPEN_EXISTING, NB | SS, IntPtr.Zero);
        if (h == (IntPtr)(-1)) throw new Win32Exception(Marshal.GetLastWin32Error());
        IntPtr b = Marshal.AllocHGlobal(block);
        try { long np;
              SetFilePointerEx(h, 0, out np, 0);
              var sw = System.Diagnostics.Stopwatch.StartNew(); long done = 0; uint n;
              while (done < total) { if (!ReadFile(h, b, (uint)block, out n, IntPtr.Zero) || n == 0) break; done += n; }
              sw.Stop();
              return string.Format("{0:N1} MiB in {1:N2} s -> {2:N2} MiB/s", done/1048576.0, sw.Elapsed.TotalSeconds, (done/1048576.0)/sw.Elapsed.TotalSeconds); }
        finally { Marshal.FreeHGlobal(b); CloseHandle(h); }
    }
}
'@
$path = 'C:\Windows\Temp\rawwarm.bin'
$bytes = [int64]$Mb * 1MB
if (-not (Test-Path $path) -or (Get-Item $path).Length -ne $bytes) {
    "creating $Mb MiB file first"
    $fs = [IO.File]::Create($path); $buf = New-Object byte[] (1MB); for ($i=0; $i -lt $Mb; $i++) { $fs.Write($buf,0,$buf.Length) }; $fs.Close()
}
for ($p = 1; $p -le $Passes; $p++) { "pass $p : " + [RawReadOnly]::RunPass($path, $bytes, 1MB) }
"done"
