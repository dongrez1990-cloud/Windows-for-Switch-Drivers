# Two unbuffered write passes + counter snapshot, to check run-to-run variance.
$ErrorActionPreference = 'Continue'
$dreg = 'HKLM:\SYSTEM\CurrentControlSet\Services\nvtsdhc\Parameters\Diag'
Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices; using System.ComponentModel;
public static class RawW
{
    const uint GENERIC_WRITE=0x40000000, CREATE_ALWAYS=2, NB=0x20000000, SS=0x08000000;
    [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
    static extern IntPtr CreateFileW(string n,uint a,uint s,IntPtr sec,uint d,uint f,IntPtr t);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool WriteFile(IntPtr h,IntPtr b,uint l,out uint w,IntPtr o);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    public static string Run(string p,long total,int block)
    {
        IntPtr h=CreateFileW(p,GENERIC_WRITE,1,IntPtr.Zero,CREATE_ALWAYS,NB|SS,IntPtr.Zero);
        if(h==(IntPtr)(-1)) throw new Win32Exception(Marshal.GetLastWin32Error());
        IntPtr b=Marshal.AllocHGlobal(block);
        try{ var sw=System.Diagnostics.Stopwatch.StartNew(); long done=0; uint n;
             while(done<total){ if(!WriteFile(h,b,(uint)block,out n,IntPtr.Zero)||n==0) break; done+=n; }
             sw.Stop();
             return string.Format("{0:N1} MiB in {1:N2} s -> {2:N2} MiB/s", done/1048576.0, sw.Elapsed.TotalSeconds,(done/1048576.0)/sw.Elapsed.TotalSeconds); }
        finally{ Marshal.FreeHGlobal(b); CloseHandle(h); }
    }
}
'@
$p='C:\Windows\Temp\rawwrite.bin'
for ($i=1; $i -le 2; $i++) {
  "pass $i : " + [RawW]::Run($p, [int64]128*1MB, 1MB)
  Start-Sleep -Seconds 3
}
Remove-Item $p -Force -ErrorAction SilentlyContinue
$k = Get-ItemProperty $dreg
"CmdErr=$($k.CommandErrors) ErrInt=$($k.ErrorInterrupts) LastErr=$($k.LastErrors) LastErrCmd=$($k.LastErrorCommand)"
"Act=$($k.ClockKhzActual) Req=$($k.ClockKhzRequested)"
