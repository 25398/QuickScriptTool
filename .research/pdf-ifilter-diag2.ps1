# Verbose IFilter diagnostic: dump every chunk's metadata and every GetText result.
param([Parameter(Mandatory=$true)][string]$Path,
      [string]$Clsid = '6C337B26-3E38-4F98-813B-FBA18BAB64F5')
$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;

[ComImport, Guid("b824b49d-22ac-4161-ac8a-9916e8fa3f7f"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IQstInit2 { [PreserveSig] int Initialize(IntPtr pstream, uint grfMode); }

[ComImport, Guid("89BCB740-6119-101A-BCB7-00DD010655AF"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IQstFilter2
{
    [PreserveSig] int Init(uint grfFlags, uint cAttributes, IntPtr aAttributes, out uint pdwFlags);
    [PreserveSig] int GetChunk(IntPtr pStat);
    [PreserveSig] int GetText(ref uint pcwcBuffer, IntPtr awcBuffer);
    [PreserveSig] int GetValue(out IntPtr ppPropValue);
    [PreserveSig] int BindRegion(int origPos, ref Guid riid, out IntPtr ppunk);
}

public static class Diag2
{
    [DllImport("ole32.dll")] static extern int CoCreateInstance(ref Guid rclsid, IntPtr pUnkOuter, uint ctx, ref Guid riid, out IntPtr ppv);
    [DllImport("shlwapi.dll", CharSet = CharSet.Unicode, ExactSpelling = true)]
    static extern int SHCreateStreamOnFileEx(string f, uint mode, uint attr, bool create, IntPtr tmpl, out IntPtr ppstm);

    public static string Run(string path, string clsid, int maxChars)
    {
        var log = new StringBuilder();
        Guid cl = new Guid(clsid), iid = new Guid("89BCB740-6119-101A-BCB7-00DD010655AF");
        IntPtr pObj, pStream;
        int hr = CoCreateInstance(ref cl, IntPtr.Zero, 1, ref iid, out pObj);
        if (hr != 0) return "CoCreateInstance 0x" + hr.ToString("X8");
        hr = SHCreateStreamOnFileEx(path, 0x20, 0, false, IntPtr.Zero, out pStream);   // STGM_READ|SHARE_DENY_WRITE
        if (hr != 0) return "SHCreateStream 0x" + hr.ToString("X8");
        var init = (IQstInit2)Marshal.GetObjectForIUnknown(pObj);
        hr = init.Initialize(pStream, 0);
        log.AppendLine("Initialize 0x" + hr.ToString("X8"));
        var filt = (IQstFilter2)init;
        uint flags = 0;
        hr = filt.Init(0, 0, IntPtr.Zero, out flags);
        log.AppendLine("Init 0x" + hr.ToString("X8") + " flags=0x" + flags.ToString("X"));

        IntPtr stat = Marshal.AllocCoTaskMem(256);
        int bufChars = 65536;
        IntPtr buf = Marshal.AllocCoTaskMem(bufChars * 2);
        var sb = new StringBuilder();
        try
        {
            for (int ci = 0; ci < 40; ci++)
            {
                hr = filt.GetChunk(stat);
                if (unchecked((uint)hr) == 0x8004170C) { log.AppendLine("chunk " + ci + ": END_OF_CHUNKS"); break; }
                if (hr != 0) { log.AppendLine("chunk " + ci + ": GetChunk hr=0x" + hr.ToString("X8")); break; }
                uint id = (uint)Marshal.ReadInt32(stat, 0);
                int brk = Marshal.ReadInt32(stat, 4);
                int fl  = Marshal.ReadInt32(stat, 8);
                int loc = Marshal.ReadInt32(stat, 12);
                log.AppendLine("chunk " + ci + ": id=" + id + " breakType=" + brk + " flags=0x" + fl.ToString("X") +
                               " locale=0x" + loc.ToString("X") + " " + (((fl & 1) != 0) ? "TEXT" : "value"));
                if ((fl & 1) == 0) continue;
                for (int k = 0; k < 5; k++)
                {
                    uint cch = (uint)bufChars;
                    hr = filt.GetText(ref cch, buf);
                    log.AppendLine("   GetText#" + k + " hr=0x" + hr.ToString("X8") + " cch=" + cch);
                    if (hr == 0 && cch > 0)
                    {
                        string s = Marshal.PtrToStringUni(buf, (int)cch);
                        log.AppendLine("      >>'" + (s.Length > 200 ? s.Substring(0, 200) + "..." : s) + "'");
                        sb.Append(s);
                        if (sb.Length >= maxChars) break;
                    }
                    else break;
                }
            }
        }
        finally { Marshal.FreeCoTaskMem(stat); Marshal.FreeCoTaskMem(buf); }
        log.AppendLine("TOTAL TEXT CHARS: " + sb.Length);
        log.AppendLine("FULL TEXT: " + sb.ToString());
        return log.ToString();
    }
}
'@

$full = (Resolve-Path $Path).Path
"file: $full"
[Diag2]::Run($full, $Clsid, 400000)
