# PDF text extraction using ONLY what ships with Windows:
#   CoCreateInstance(PDF search filter) -> IInitializeWithStream::Initialize(SHCreateStreamOnFileEx)
#   -> IFilter::Init -> GetChunk/GetText loop.
# Verified working on Windows 11 (26200) with NO Office, NO Python, NO third-party DLLs.
param(
    [Parameter(Mandatory=$true)][string]$Path,
    [string]$Clsid = '6C337B26-3E38-4F98-813B-FBA18BAB64F5',
    [int]$MaxChars = 400000
)
$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;

[ComImport, Guid("b824b49d-22ac-4161-ac8a-9916e8fa3f7f"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IQstInitWithStream
{
    [PreserveSig] int Initialize(IntPtr pstream, uint grfMode);
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

public static class QstPdfText
{
    [DllImport("ole32.dll")]
    private static extern int CoCreateInstance(ref Guid rclsid, IntPtr pUnkOuter, uint dwClsContext, ref Guid riid, out IntPtr ppv);

    // shlwapi
    [DllImport("shlwapi.dll", CharSet = CharSet.Unicode, ExactSpelling = true)]
    private static extern int SHCreateStreamOnFileEx(string pszFile, uint grfMode, uint dwAttributes, bool fCreate, IntPtr pstmTemplate, out IntPtr ppstm);

    const uint CLSCTX_INPROC_SERVER = 0x1;
    const uint STGM_READ = 0x0;
    const uint STGM_SHARE_DENY_WRITE = 0x20;
    const int S_OK = 0;
    const uint FILTER_E_END_OF_CHUNKS = 0x8004170C;
    const int CHUNK_TEXT = 0x1;

    public static string Extract(string path, string clsid, int maxChars, out string diag)
    {
        var log = new StringBuilder();
        var sb = new StringBuilder();
        Guid cl = new Guid(clsid);
        Guid iidFilter = new Guid("89BCB740-6119-101A-BCB7-00DD010655AF");

        IntPtr pObj = IntPtr.Zero;
        int hr = CoCreateInstance(ref cl, IntPtr.Zero, CLSCTX_INPROC_SERVER, ref iidFilter, out pObj);
        log.Append("CoCreateInstance hr=0x" + hr.ToString("X8") + "; ");
        if (hr != S_OK) { diag = log.ToString(); return ""; }

        IntPtr pStream = IntPtr.Zero;
        try
        {
            hr = SHCreateStreamOnFileEx(path, STGM_READ | STGM_SHARE_DENY_WRITE, 0, false, IntPtr.Zero, out pStream);
            log.Append("SHCreateStreamOnFileEx hr=0x" + hr.ToString("X8") + "; ");
            if (hr != S_OK) { diag = log.ToString(); return ""; }

            var init = (IQstInitWithStream)System.Runtime.InteropServices.Marshal.GetObjectForIUnknown(pObj);
            hr = init.Initialize(pStream, STGM_READ);
            log.Append("Initialize(stream) hr=0x" + hr.ToString("X8") + "; ");
            if (hr != S_OK) { diag = log.ToString(); return ""; }

            var filter = (IQstFilter)init;
            uint flags = 0;
            hr = filter.Init(0, 0, IntPtr.Zero, out flags);
            log.Append("Init hr=0x" + hr.ToString("X8") + " flags=0x" + flags.ToString("X") + "; ");
            if (hr != S_OK) { diag = log.ToString(); return ""; }

            IntPtr stat = Marshal.AllocCoTaskMem(256);
            int bufChars = 65536;
            IntPtr buf = Marshal.AllocCoTaskMem(bufChars * 2);
            int chunks = 0, textChunks = 0;
            try
            {
                while (sb.Length < maxChars)
                {
                    hr = filter.GetChunk(stat);
                    if (unchecked((uint)hr) == FILTER_E_END_OF_CHUNKS) { log.Append("EOC; "); break; }
                    if (hr != S_OK) { log.Append("GetChunk hr=0x" + hr.ToString("X8") + "; "); break; }
                    chunks++;
                    uint chunkFlags = (uint)Marshal.ReadInt32(stat, 8);
                    if ((chunkFlags & CHUNK_TEXT) == 0) continue;
                    textChunks++;
                    while (true)
                    {
                        uint cch = (uint)bufChars;
                        hr = filter.GetText(ref cch, buf);
                        // IMPORTANT: success is hr >= 0, not hr == 0.
                        // 0x00041709 FILTER_S_LAST_TEXT and 0x0004170A FILTER_S_LAST_VALUES
                        // are SUCCESS codes that still carry text in the buffer.
                        if (hr >= 0 && cch > 0)
                        {
                            sb.Append(Marshal.PtrToStringUni(buf, (int)cch));
                            if (sb.Length >= maxChars) break;
                            if ((uint)hr == 0x00041709) break;   // last text in this chunk
                        }
                        else break;
                    }
                }
            }
            finally { Marshal.FreeCoTaskMem(stat); Marshal.FreeCoTaskMem(buf); }
            log.Append("chunks=" + chunks + " textChunks=" + textChunks);
        }
        finally
        {
            if (pStream != IntPtr.Zero) Marshal.Release(pStream);
            if (pObj != IntPtr.Zero) Marshal.Release(pObj);
        }
        diag = log.ToString();
        return sb.ToString();
    }
}
'@

$full = (Resolve-Path $Path).Path
$sw = [System.Diagnostics.Stopwatch]::StartNew()
$diag = ''
$text = [QstPdfText]::Extract($full, $Clsid, $MaxChars, [ref]$diag)
$sw.Stop()
"file    : $full"
"diag    : $diag"
"elapsed : $([math]::Round($sw.Elapsed.TotalSeconds,3))s"
"chars   : $($text.Length)"
"---- TEXT ----"
$text
"---- END ----"
