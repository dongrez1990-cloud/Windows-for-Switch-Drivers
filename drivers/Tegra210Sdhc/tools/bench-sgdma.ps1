# SG-DMA validation: error counters, write speed, and a byte-exact write/read check.
$ErrorActionPreference = 'Continue'
$dreg = 'HKLM:\SYSTEM\CurrentControlSet\Services\nvtsdhc\Parameters\Diag'
$k = Get-ItemProperty $dreg
"--- Diag errors ---"
"SgReq=$($k.SgDmaRequests) SgBlk=$($k.SgDmaBlocks) SgErr=$($k.SgDmaErrors) SgErrStatus=$($k.SgDmaErrStatus) SgAdmaErr=$($k.SgDmaAdmaErr)"
"CmdErr=$($k.CommandErrors) ErrInt=$($k.ErrorInterrupts) LastErr=$($k.LastErrors) LastErrCmd=$($k.LastErrorCommand) LastErrKind=$($k.LastErrorKind)"
"Bus=$($k.BusSpeed) Act=$($k.ClockKhzActual) Base=$($k.BaseClockKhz) RegClk=$($k.RegClockControl) RegHc=$($k.RegHostControl)"
"CarBefore=$($k.CarSdmmc1Before) CarAfter=$($k.CarSdmmc1After)"

Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices; using System.ComponentModel;
public static class RawX
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

    static IntPtr Open(string p, uint a, uint disp)
    {
        IntPtr h=CreateFileW(p,a,1,IntPtr.Zero,disp,NB|SS,IntPtr.Zero);
        if(h==(IntPtr)(-1)) throw new Win32Exception(Marshal.GetLastWin32Error());
        return h;
    }
    public static string WritePattern(string p, int mb)
    {
        IntPtr h=Open(p,GENERIC_WRITE,CREATE_ALWAYS);
        int blk=1<<20; IntPtr b=Marshal.AllocHGlobal(blk);
        try {
            var rnd=new Random(12345);
            var sw=System.Diagnostics.Stopwatch.StartNew();
            for(int i=0;i<mb;i++){
                byte[] tmp=new byte[blk]; rnd.NextBytes(tmp);
                Marshal.Copy(tmp,0,b,blk);
                uint w; if(!WriteFile(h,b,(uint)blk,out w,IntPtr.Zero)) return "write failed "+Marshal.GetLastWin32Error();
            }
            FlushFileBuffers(h);
            sw.Stop();
            return string.Format("{0} MiB in {1:N2} s -> {2:N2} MiB/s (incl. flush)", mb, sw.Elapsed.TotalSeconds, mb/sw.Elapsed.TotalSeconds);
        } finally { Marshal.FreeHGlobal(b); CloseHandle(h); }
    }
    public static string ReadHash(string p, int mb)
    {
        IntPtr h=Open(p,GENERIC_READ,OPEN_EXISTING);
        int blk=1<<20; IntPtr b=Marshal.AllocHGlobal(blk);
        try {
            long np; SetFilePointerEx(h,0,out np,0);
            var sha=System.Security.Cryptography.SHA256.Create();
            var sw=System.Diagnostics.Stopwatch.StartNew();
            byte[] tmp=new byte[blk];
            for(int i=0;i<mb;i++){
                uint n; if(!ReadFile(h,b,(uint)blk,out n,IntPtr.Zero)) return "read failed "+Marshal.GetLastWin32Error();
                Marshal.Copy(b,tmp,0,(int)n); sha.TransformBlock(tmp,0,(int)n,null,0);
            }
            sha.TransformFinalBlock(new byte[0],0,0);
            sw.Stop();
            string hex=BitConverter.ToString(sha.Hash).Replace("-","");
            return string.Format("{0} MiB in {1:N2} s -> {2:N2} MiB/s  sha256={3}", mb, sw.Elapsed.TotalSeconds, mb/sw.Elapsed.TotalSeconds, hex.Substring(0,16));
        } finally { Marshal.FreeHGlobal(b); CloseHandle(h); }
    }
}
'@
$p='C:\Windows\Temp\sgcheck.bin'
$out = @()
$out += "write: " + [RawX]::WritePattern($p, 64)
Start-Sleep -Seconds 1
$out += "read : " + [RawX]::ReadHash($p, 64)
# Independent hash of the same file through the normal (cached) path for comparison
$out += "os-hash: " + (Get-FileHash $p -Algorithm SHA256).Hash.Substring(0,16)
$out -join "`n"
Remove-Item $p -Force -ErrorAction SilentlyContinue
$k = Get-ItemProperty $dreg
"--- after ---"
"SgReq=$($k.SgDmaRequests) SgBlk=$($k.SgDmaBlocks) SgErr=$($k.SgDmaErrors) CmdErr=$($k.CommandErrors)"
