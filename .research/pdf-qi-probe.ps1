# Probe which interfaces the PDF search filter object actually supports.
$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class QiProbe
{
    [DllImport("ole32.dll")]
    public static extern int CoCreateInstance(ref Guid rclsid, IntPtr pUnkOuter, uint dwClsContext, ref Guid riid, out IntPtr ppv);
    public static int QI(IntPtr p, string iid)
    {
        Guid g = new Guid(iid); IntPtr o;
        int hr = Marshal.QueryInterface(p, ref g, out o);
        if (hr == 0 && o != IntPtr.Zero) Marshal.Release(o);
        return hr;
    }
}
'@

$targets = [ordered]@{
  'IUnknown'                = '00000000-0000-0000-C000-000000000046'
  'IFilter'                 = '89BCB740-6119-101A-BCB7-00DD010655AF'
  'IPersistFile'            = '0000010b-0000-0000-C000-000000000046'
  'IPersistStream'          = '00000109-0000-0000-C000-000000000046'
  'IPersistStreamInit'      = '7FD52380-4E07-101B-AE2D-08002B2EC713'
  'IPersist'                = '0000010c-0000-0000-C000-000000000046'
  'IInitializeWithFile'     = 'b7d14566-0509-4cce-a71f-0a554233bd9b'
  'IInitializeWithStream'   = 'b824b49d-22ac-4161-ac8a-9916e8fa3f7f'
  'IInitializeWithItem'     = '7f73be3f-fb79-493c-a6c7-7ee14e245841'
  'IObjectWithSite'         = 'fc4801a3-2ba9-11cf-a229-00aa003d7352'
  'IStream'                 = '0000000c-0000-0000-C000-000000000046'
  'IPropertyStore'          = '886d8eeb-8cf2-4446-8d02-cdba1dbdcf99'
  'IPropertySetStorage'     = '0000013a-0000-0000-C000-000000000046'
  'IFilter_Alt'             = '89BCB740-6119-101A-BCB7-00DD010655AF'
}

foreach ($clsidStr in '6C337B26-3E38-4F98-813B-FBA18BAB64F5') {
  "=== CLSID {$clsidStr} ==="
  $clsid = [Guid]$clsidStr
  $iidUnk = [Guid]'00000000-0000-0000-C000-000000000046'
  $pv = [IntPtr]::Zero
  $hr = [QiProbe]::CoCreateInstance([ref]$clsid, [IntPtr]::Zero, 1, [ref]$iidUnk, [ref]$pv)
  "CoCreateInstance(IUnknown) hr=0x$($hr.ToString('X8'))"
  if ($pv -ne [IntPtr]::Zero) {
    foreach ($k in $targets.Keys) {
      $h = [QiProbe]::QI($pv, $targets[$k])
      $mark = if ($h -eq 0) { 'YES' } else { ' - ' }
      "  {0,-24} {1}  (0x{2:X8})" -f $k, $mark, $h
    }
    [void][System.Runtime.InteropServices.Marshal]::Release($pv)
  }
}
