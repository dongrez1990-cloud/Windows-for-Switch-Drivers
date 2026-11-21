$ErrorActionPreference = 'Continue'
Start-Sleep -Seconds 200
Add-Type -Namespace Safe -Name Disp -MemberDefinition @"
[System.Runtime.InteropServices.DllImport("user32.dll", CharSet=System.Runtime.InteropServices.CharSet.Unicode)] public static extern int ChangeDisplaySettingsEx(string d, ref DEVMODE m, System.IntPtr h, uint f, System.IntPtr p);
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
$m = New-Object Safe.Disp+DEVMODE
$m.dmSize = [uint16][System.Runtime.InteropServices.Marshal]::SizeOf($m)
$m.dmPelsWidth = 1280; $m.dmPelsHeight = 720; $m.dmBitsPerPel = 32; $m.dmDisplayOrientation = 1
$m.dmFields = 0x80000 -bor 0x100000 -bor 0x80 -bor 0x40000
$rc = [Safe.Disp]::ChangeDisplaySettingsEx($null, [ref]$m, [IntPtr]::Zero, 0, [IntPtr]::Zero)
"display-safety restore rc=$rc at $(Get-Date -Format 'HH:mm:ss')" | Add-Content C:\SwitchWOA\display-safety.log
