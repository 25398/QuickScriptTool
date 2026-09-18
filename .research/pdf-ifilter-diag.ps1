# Diagnostic: figure out why the PDF IFilter path returns E_NOINTERFACE.
$ErrorActionPreference = 'Stop'

"apartment: $([System.Threading.Thread]::CurrentThread.GetApartmentState())"
"process  : $([System.Environment]::Is64BitProcess) (64-bit=$([Environment]::Is64BitOperatingSystem))"

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class FDiag
{
    [DllImport("query.dll", CharSet = CharSet.Unicode, ExactSpelling = true)]
    public static extern int LoadIFilter(string pwcsPath, IntPtr pUnkOuter, out IntPtr ppIUnk);

    [DllImport("ole32.dll")]
    public static extern int CoInitializeEx(IntPtr pvReserved, uint dwCoInit);

    [DllImport("ole32.dll")]
    public static extern int CoCreateInstance(ref Guid rclsid, IntPtr pUnkOuter, uint dwClsContext, ref Guid riid, out IntPtr ppv);

    [DllImport("ole32.dll")]
    public static extern int CoGetClassObject(ref Guid rclsid, uint dwClsContext, IntPtr pvReserved, ref Guid riid, out IntPtr ppv);

    [DllImport("ole32.dll")]
    public static extern int CLSIDFromString(string lpsz, out Guid pclsid);

    public const uint CLSCTX_INPROC_SERVER = 0x1;
    public const uint CLSCTX_LOCAL_SERVER  = 0x4;

    // IPersistFile
    [ComImport, Guid("0000010b-0000-0000-C000-000000000046"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    public interface IPersistFile
    {
        [PreserveSig] int GetClassID(out Guid pClassID);
        [PreserveSig] int IsDirty();
        [PreserveSig] int Load([MarshalAs(UnmanagedType.LPWStr)] string pszFileName, uint dwMode);
        [PreserveSig] int Save([MarshalAs(UnmanagedType.LPWStr)] string pszFileName, bool fRemember);
        [PreserveSig] int SaveCompleted([MarshalAs(UnmanagedType.LPWStr)] string pszFileName);
        [PreserveSig] int GetCurFile(out IntPtr ppszFileName);
    }

    [ComImport, Guid("89BCB740-6119-101A-BCB7-00DD010655AF"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    public interface IFilter
    {
        [PreserveSig] int Init(uint grfFlags, uint cAttributes, IntPtr aAttributes, out uint pdwFlags);
        [PreserveSig] int GetChunk(IntPtr pStat);
        [PreserveSig] int GetText(ref uint pcwcBuffer, IntPtr awcBuffer);
        [PreserveSig] int GetValue(out IntPtr ppPropValue);
        [PreserveSig] int BindRegion(int origPos, ref Guid riid, out IntPtr ppunk);
    }

    public static string QI(IntPtr p, string iid)
    {
        Guid g; CLSIDFromString(iid, out g);
        IntPtr outPtr;
        int hr = Marshal.QueryInterface(p, ref g, out outPtr);
        if (hr == 0 && outPtr != IntPtr.Zero) Marshal.Release(outPtr);
        return "hr=0x" + hr.ToString("X8");
    }
}
'@

$pdf = 'D:\other\software\.research\fixtures\handmade.pdf'
$iidIFilter = '89BCB740-6119-101A-BCB7-00DD010655AF'
$iidIPersistFile = '0000010b-0000-0000-C000-000000000046'
$iidIUnknown = '00000000-0000-0000-C000-000000000046'

"--- 1) LoadIFilter -> raw IUnknown, then manual QI for IFilter ---"
$p = [IntPtr]::Zero
$hr = [FDiag]::LoadIFilter($pdf, [IntPtr]::Zero, [ref]$p)
"LoadIFilter hr=0x$($hr.ToString('X8')) ptr=$p"
if ($p -ne [IntPtr]::Zero) {
    "  QI IFilter      : $([FDiag]::QI($p,$iidIFilter))"
    "  QI IPersistFile : $([FDiag]::QI($p,$iidIPersistFile))"
    [void][System.Runtime.InteropServices.Marshal]::Release($p)
}

"--- 2) CoCreateInstance({6C337B26-...}, IFilter) ---"
$clsid = [Guid]'6C337B26-3E38-4F98-813B-FBA18BAB64F5'
$iid   = [Guid]$iidIFilter
$pv = [IntPtr]::Zero
$hr2 = [FDiag]::CoCreateInstance([ref]$clsid, [IntPtr]::Zero, [FDiag]::CLSCTX_INPROC_SERVER, [ref]$iid, [ref]$pv)
"CoCreateInstance(INPROC) hr=0x$($hr2.ToString('X8')) ptr=$pv"
if ($pv -eq [IntPtr]::Zero) {
    $hr2b = [FDiag]::CoCreateInstance([ref]$clsid, [IntPtr]::Zero, [FDiag]::CLSCTX_LOCAL_SERVER, [ref]$iid, [ref]$pv)
    "CoCreateInstance(LOCAL)  hr=0x$($hr2b.ToString('X8')) ptr=$pv"
}
if ($pv -ne [IntPtr]::Zero) {
    $pf = [Runtime.InteropServices.Marshal]::GetObjectForIUnknown($pv)
    "  got: $($pf.GetType().FullName)"
    try {
        $f2 = [FDiag+IFilter]$pf
        $fl = 0
        $h = $f2.Init(0,0,[IntPtr]::Zero,[ref]$fl)
        "  IFilter.Init hr=0x$($h.ToString('X8'))"
    } catch { "  cast to IFilter failed: $($_.Exception.Message)" }
    try {
        $pf2 = [FDiag+IPersistFile]$pf
        $h = $pf2.Load($pdf, 0)
        "  IPersistFile.Load hr=0x$($h.ToString('X8'))"
    } catch { "  cast to IPersistFile failed: $($_.Exception.Message)" }
    [void][System.Runtime.InteropServices.Marshal]::Release($pv)
}

"--- 3) is the CLSID actually registered in the 64-bit hive? ---"
foreach ($k in "HKLM:\SOFTWARE\Classes\CLSID\{6C337B26-3E38-4F98-813B-FBA18BAB64F5}",
               "HKLM:\SOFTWARE\Classes\CLSID\{6C337B26-3E38-4F98-813B-FBA18BAB64F5}\InprocServer32") {
  if (Test-Path $k) { "OK   $k" ; Get-ItemProperty $k | Select-Object '(default)','ThreadingModel' | Format-List }
  else { "MISS $k" }
}
