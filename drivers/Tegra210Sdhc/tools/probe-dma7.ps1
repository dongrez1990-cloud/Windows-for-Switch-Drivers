# Deterministic DMA probe trigger: one unbuffered read of exactly 9 blocks
# (4608 bytes). The driver only runs the ADMA-vs-PIO probe on that size.
$ErrorActionPreference = 'Continue'
$dreg = 'HKLM:\SYSTEM\CurrentControlSet\Services\nvtsdhc\Parameters\Diag'
Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices; using System.ComponentModel;
public static class BlkRead
{
    const uint GENERIC_WRITE=0x40000000, GENERIC_READ=0x80000000;
    const uint CREATE_ALWAYS=2, OPEN_EXISTING=3, NB=0x20000000, SS=0x08000000;
    [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
    static extern IntPtr CreateFileW(string n,uint a,uint s,IntPtr sec,uint d,uint f,IntPtr t);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool ReadFile(IntPtr h,IntPtr b,uint l,out uint r,IntPtr o);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool WriteFile(IntPtr h,IntPtr b,uint l,out uint w,IntPtr o);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool SetFilePointerEx(IntPtr h,long d,out long np,uint m);

    public static string Make(string p, int bytes)
    {
        IntPtr h=CreateFileW(p,GENERIC_WRITE,1,IntPtr.Zero,CREATE_ALWAYS,NB|SS,IntPtr.Zero);
        if(h==(IntPtr)(-1)) return "create failed " + Marshal.GetLastWin32Error();
        IntPtr b=Marshal.AllocHGlobal(bytes);
        try { uint w; bool ok=WriteFile(h,b,(uint)bytes,out w,IntPtr.Zero); return ok ? ("wrote " + w) : ("write failed " + Marshal.GetLastWin32Error()); }
        finally { Marshal.FreeHGlobal(b); CloseHandle(h); }
    }
    public static string Read(string p, int bytes)
    {
        IntPtr h=CreateFileW(p,GENERIC_READ,1,IntPtr.Zero,OPEN_EXISTING,NB|SS,IntPtr.Zero);
        if(h==(IntPtr)(-1)) return "open failed " + Marshal.GetLastWin32Error();
        IntPtr b=Marshal.AllocHGlobal(bytes);
        try { long np; SetFilePointerEx(h,0,out np,0); uint n;
              bool ok=ReadFile(h,b,(uint)bytes,out n,IntPtr.Zero);
              return ok ? ("read " + n) : ("read failed " + Marshal.GetLastWin32Error()); }
        finally { Marshal.FreeHGlobal(b); CloseHandle(h); }
    }
}
'@
$p = 'C:\Windows\Temp\blk9.bin'
"create: " + [BlkRead]::Make($p, 4608)
Start-Sleep -Seconds 2
$k = Get-ItemProperty $dreg
"before: DataCommands=$($k.DataCommands) DataStartBlocks=$($k.DataStartBlocks) ProbeCalls=[$($k.UhsDmaProbeCalls)] DmaTest=$($k.UhsDmaTest)"
Start-Sleep -Seconds 2
"trigger: " + [BlkRead]::Read($p, 4608)
Start-Sleep -Seconds 3
$k = Get-ItemProperty $dreg
"after : DataStartBlocks=$($k.DataStartBlocks) ProbeCalls=[$($k.UhsDmaProbeCalls)] Skip=[$($k.UhsDmaProbeSkip)] DmaTest=$($k.UhsDmaTest) DmaOk=$($k.UhsDmaOk)"
"detail: StDma=$($k.UhsDmaStDma) StPio=$($k.UhsDmaStPio) Sig=$($k.UhsDmaSig) Is=$($k.UhsDmaIs) AdmaErr=$($k.UhsDmaAdma) Ps=$($k.UhsDmaPs)"
"Smmu=$($k.UhsSmmu)/$($k.UhsSmmu2)/$($k.UhsSmmu3)/$($k.UhsSmmu4) ClockAct=$($k.ClockKhzActual) CmdErr=$($k.CommandErrors)"
Remove-Item $p -Force -ErrorAction SilentlyContinue
