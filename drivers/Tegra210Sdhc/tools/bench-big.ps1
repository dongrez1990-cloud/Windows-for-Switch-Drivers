# 256 MiB write+read stress with error counters around it (SG-DMA validation).
$ErrorActionPreference = 'Continue'
$dreg = 'HKLM:\SYSTEM\CurrentControlSet\Services\nvtsdhc\Parameters\Diag'
Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices; using System.ComponentModel;
public static class BigChk
{
    const uint GENERIC_WRITE=0x40000000, GENERIC_READ=0x80000000;
    const uint CREATE_ALWAYS=2, OPEN_EXISTING=3, NB=0x20000000, SS=0x08000000;
    [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
    static extern IntPtr CreateFileW(string n,uint a,uint s,IntPtr sec,uint d,uint f,IntPtr t);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool ReadFile(IntPtr h,IntPtr b,uint l,out uint r,IntPtr o);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool WriteFile(IntPtr h,IntPtr b,uint l,out uint w,IntPtr o);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool SetFilePointerEx(IntPtr h,long d,out long np,uint m);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool FlushFileBuffers(IntPtr h);
    static IntPtr Open(string p,uint a,uint d)
    {
        IntPtr h=CreateFileW(p,a,1,IntPtr.Zero,d,NB|SS,IntPtr.Zero);
        if(h==(IntPtr)(-1)) throw new Win32Exception(Marshal.GetLastWin32Error());
        return h;
    }
    public static string Write(string p,int mb)
    {
        int blk=1<<20; IntPtr b=Marshal.AllocHGlobal(blk);
        try {
            var rnd=new Random(7); var tmp=new byte[blk];
            IntPtr h=Open(p,GENERIC_WRITE,CREATE_ALWAYS);
            var sw=System.Diagnostics.Stopwatch.StartNew();
            for(int i=0;i<mb;i++){ rnd.NextBytes(tmp); Marshal.Copy(tmp,0,b,blk); uint w;
                if(!WriteFile(h,b,(uint)blk,out w,IntPtr.Zero)) return "write fail "+Marshal.GetLastWin32Error(); }
            FlushFileBuffers(h); sw.Stop(); CloseHandle(h);
            return string.Format("write {0} MiB  {1:N2}s  {2:N2} MiB/s (incl flush)", mb, sw.Elapsed.TotalSeconds, mb/sw.Elapsed.TotalSeconds);
        } finally { Marshal.FreeHGlobal(b); }
    }
    public static string Read(string p,int mb)
    {
        int blk=1<<20; IntPtr b=Marshal.AllocHGlobal(blk);
        try {
            IntPtr h=Open(p,GENERIC_READ,OPEN_EXISTING);
            long np; SetFilePointerEx(h,0,out np,0);
            var sw=System.Diagnostics.Stopwatch.StartNew();
            for(int i=0;i<mb;i++){ uint n; if(!ReadFile(h,b,(uint)blk,out n,IntPtr.Zero)) return "read fail "+Marshal.GetLastWin32Error(); }
            sw.Stop(); CloseHandle(h);
            return string.Format("read  {0} MiB  {1:N2}s  {2:N2} MiB/s", mb, sw.Elapsed.TotalSeconds, mb/sw.Elapsed.TotalSeconds);
        } finally { Marshal.FreeHGlobal(b); }
    }
}
'@
$k0 = Get-ItemProperty $dreg
"before: SgErr=$($k0.SgDmaErrors) CmdErr=$($k0.CommandErrors) SgBlk=$($k0.SgDmaBlocks)"
$p = 'C:\Windows\Temp\bigchk.bin'
[BigChk]::Write($p, 256)
"os-hash: " + (Get-FileHash $p -Algorithm SHA256).Hash.Substring(0,16)
[BigChk]::Read($p, 256)
Remove-Item $p -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2
$k = Get-ItemProperty $dreg
"after : SgErr=$($k.SgDmaErrors) CmdErr=$($k.CommandErrors) SgBlk=$($k.SgDmaBlocks) SgAdmaErr=$($k.SgDmaAdmaErr) ClockAct=$($k.ClockKhzActual)"
