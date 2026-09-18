# PDF text extraction through the Windows Search IFilter, created directly by CLSID.
# Top-level ComImport interfaces (nested ones can't be cast from __ComObject in PS 5.1).
param(
    [Parameter(Mandatory=$true)][string]$Path,
    [string]$Clsid = '6C337B26-3E38-4F98-813B-FBA18BAB64F5',   # Windows 11 PDF "Reader Search Handler"
    [int]$MaxChars = 200000
)
$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;

[ComImport, Guid("0000010b-0000-0000-C000-000000000046"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IQstPersistFile
{
    [PreserveSig] int GetClassID(out Guid pClassID);
    [PreserveSig] int IsDirty();
    [PreserveSig] int Load([MarshalAs(UnmanagedType.LPWStr)] string pszFileName, uint dwMode);
    [PreserveSig] int Save([MarshalAs(UnmanagedType.LPWStr)] string pszFileName, bool fRemember);
    [PreserveSig] int SaveCompleted([MarshalAs(UnmanagedType.LPWStr)] string pszFileName);
    [PreserveSig] int GetCurFile(out IntPtr ppszFileName);
}

[ComImport, Guid("89BCB740-6119-101A-BCB7-00DD010655AF"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IQstFilter
{
    [PreserveSig] int Init(uint grfFlags, uint cAttributes, IntPtr aAttributes, out uint pdwFlags);
    [PreserveSig] int GetChunk(IntPtr pStat);
    [PreserveSig] int GetText(ref uint pcwcBuffer, IntPtr awcBuffer);
    [PreserveSig] int GetValue(out IntPtr ppPropValue);
    [PreserveSig] int BindRegion(int origPos, ref Guid riid, out IntPtr ppunk);
}

public static class QstFilterHost
{
    [DllImport("ole32.dll")]
    private static extern int CoCreateInstance(ref Guid rclsid, IntPtr pUnkOuter, uint dwClsContext, ref Guid riid, out IntPtr ppv);

    const uint CLSCTX_INPROC_SERVER = 0x1;
    const int S_OK = 0;
    const uint FILTER_E_END_OF_CHUNKS = 0x8004170C;
    const int CHUNK_TEXT = 0x1;

    public static string Extract(string path, string clsid, int maxChars, out string diag)
    {
        var sb = new StringBuilder();
        var log = new StringBuilder();
        Guid cl = new Guid(clsid);

        // create the filter object asking for IPersistFile first (it must be loaded) ---
        Guid iidPersist = new Guid("0000010b-0000-0000-C000-000000000046");
        IntPtr pPF = IntPtr.Zero;
        int hr = CoCreateInstance(ref cl, IntPtr.Zero, CLSCTX_INPROC_SERVER, ref iidPersist, out pPF);
        log.Append("CoCreateInstance(IPersistFile) hr=0x" + hr.ToString("X8") + "; ");
        if (hr != S_OK) { diag = log.ToString(); return ""; }

        var pf = (IQstPersistFile)Marshal.GetObjectForIUnknown(pPF);
        Marshal.Release(pPF);
        hr = pf.Load(path, 0);     // STGM_READ
        log.Append("IPersistFile.Load hr=0x" + hr.ToString("X8") + "; ");
        if (hr != S_OK) { diag = log.ToString(); return ""; }

        var filter = (IQstFilter)pf;
        uint flags = 0;
        hr = filter.Init(0, 0, IntPtr.Zero, out flags);
        log.Append("Init hr=0x" + hr.ToString("X8") + " flags=0x" + flags.ToString("X") + "; ");
        if (hr != S_OK) { diag = log.ToString(); return ""; }

        IntPtr stat = Marshal.AllocCoTaskMem(256);
        int bufChars = 65536;
        IntPtr buf = Marshal.AllocCoTaskMem(bufChars * 2);
        int chunks = 0;
        try
        {
            while (sb.Length < maxChars)
            {
                hr = filter.GetChunk(stat);
                if (unchecked((uint)hr) == FILTER_E_END_OF_CHUNKS) { log.Append("end-of-chunks; "); break; }
                if (hr != S_OK) { log.Append("GetChunk hr=0x" + hr.ToString("X8") + "; "); break; }
                chunks++;
                uint chunkFlags = (uint)Marshal.ReadInt32(stat, 8);
                if ((chunkFlags & CHUNK_TEXT) == 0) continue;
                while (true)
                {
                    uint cch = (uint)bufChars;
                    hr = filter.GetText(ref cch, buf);
                    if (hr == S_OK && cch > 0) { sb.Append(Marshal.PtrToStringUni(buf, (int)cch)); if (sb.Length >= maxChars) break; }
                    else break;
                }
            }
        }
        finally
        {
            Marshal.FreeCoTaskMem(stat);
            Marshal.FreeCoTaskMem(buf);
        }
        log.Append("chunks=" + chunks);
        diag = log.ToString();
        return sb.ToString();
    }
}
'@

$full = (Resolve-Path $Path).Path
$sw = [System.Diagnostics.Stopwatch]::StartNew()
$diag = ''
$text = [QstFilterHost]::Extract($full, $Clsid, $MaxChars, [ref]$diag)
$sw.Stop()
"file    : $full"
"clsid   : $Clsid"
"diag    : $diag"
"elapsed : $([math]::Round($sw.Elapsed.TotalSeconds,3))s"
"chars   : $($text.Length)"
"---- TEXT ----"
$text
"---- END ----"
