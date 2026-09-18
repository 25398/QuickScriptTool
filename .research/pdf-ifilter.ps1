# PDF (and any other registered format) text extraction via the Windows Search IFilter,
# reached through LoadIFilter() in query.dll. No Office, no Python, no third-party DLLs.
#
# On Windows 11 the .pdf PersistentHandler {1AA9BF05-...} maps IID_IFilter to
# {6C337B26-3E38-4F98-813B-FBA18BAB64F5} "Reader Search Handler" in Windows.Data.Pdf.dll.
param(
    [Parameter(Mandatory=$true)][string]$Path,
    [int]$MaxChars = 200000
)
$ErrorActionPreference = 'Stop'

if (-not ('QstIFilter' -as [type])) {
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class QstIFilter
{
    [DllImport("query.dll", CharSet = CharSet.Unicode, ExactSpelling = true)]
    private static extern int LoadIFilter(string pwcsPath, IntPtr pUnkOuter, out IFilter ppIUnk);

    [DllImport("query.dll", CharSet = CharSet.Unicode, ExactSpelling = true)]
    private static extern int LoadIFilter_(string pwcsPath, IntPtr pUnkOuter, out IntPtr ppIUnk);

    // IID_IFilter
    [ComImport, Guid("89BCB740-6119-101A-BCB7-00DD010655AF"),
     InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    public interface IFilter
    {
        [PreserveSig] int Init(uint grfFlags, uint cAttributes, IntPtr aAttributes, out uint pdwFlags);
        [PreserveSig] int GetChunk(IntPtr pStat);                 // STAT_CHUNK* (treated as opaque)
        [PreserveSig] int GetText(ref uint pcwcBuffer, IntPtr awcBuffer);
        [PreserveSig] int GetValue(out IntPtr ppPropValue);
        [PreserveSig] int BindRegion(int origPos, ref Guid riid, out IntPtr ppunk);
    }

    const int S_OK = 0;
    const uint FILTER_E_END_OF_CHUNKS  = 0x8004170C;
    const uint FILTER_E_NO_MORE_TEXT   = 0x80041705;
    const uint FILTER_E_NO_TEXT        = 0x80041706;
    const uint FILTER_E_ACCESS         = 0x80041703;
    const uint FILTER_E_EMBEDDING_UNAVAILABLE = 0x8004170B;
    const uint FILTER_E_LINK_UNAVAILABLE      = 0x8004170E;
    const int CHUNK_TEXT = 0x1;

    public static string Extract(string path, int maxChars)
    {
        IFilter filter;
        int hr = LoadIFilter(path, IntPtr.Zero, out filter);
        if (hr != S_OK || filter == null) return "<<LoadIFilter failed hr=0x" + hr.ToString("X8") + ">>";

        uint flags = 0;
        hr = filter.Init(0, 0, IntPtr.Zero, out flags);
        if (hr != S_OK) return "<<Init failed hr=0x" + hr.ToString("X8") + ">>";

        var sb = new StringBuilder();
        IntPtr stat = Marshal.AllocCoTaskMem(256);
        int bufChars = 65536;
        IntPtr buf = Marshal.AllocCoTaskMem(bufChars * 2);
        try
        {
            while (sb.Length < maxChars)
            {
                hr = filter.GetChunk(stat);
                if (unchecked((uint)hr) == FILTER_E_END_OF_CHUNKS) break;
                if (hr != S_OK) break;

                // STAT_CHUNK.flags is the 3rd ULONG (offset 8)
                uint chunkFlags = (uint)Marshal.ReadInt32(stat, 8);
                bool isText = (chunkFlags & CHUNK_TEXT) != 0;
                if (!isText)
                {
                    // skip value-only chunks (metadata); GetValue would return a PROPVARIANT
                    continue;
                }

                while (true)
                {
                    uint cch = (uint)bufChars;
                    hr = filter.GetText(ref cch, buf);
                    if (hr == S_OK && cch > 0)
                    {
                        sb.Append(Marshal.PtrToStringUni(buf, (int)cch));
                        if (sb.Length >= maxChars) break;
                    }
                    else break;   // NO_MORE_TEXT / NO_TEXT / error => next chunk
                }
            }
        }
        finally
        {
            Marshal.FreeCoTaskMem(stat);
            Marshal.FreeCoTaskMem(buf);
            Marshal.ReleaseComObject(filter);
        }
        return sb.ToString();
    }

    public static string LastStatus = "";
}
'@
}

$full = (Resolve-Path $Path).Path
$sw = [System.Diagnostics.Stopwatch]::StartNew()
$text = [QstIFilter]::Extract($full, $MaxChars)
$sw.Stop()

"file      : $full"
"elapsed   : $([math]::Round($sw.Elapsed.TotalSeconds,2))s"
"chars     : $($text.Length)"
"---- TEXT ----"
$text
"---- END ----"
