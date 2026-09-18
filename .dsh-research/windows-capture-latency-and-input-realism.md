# Windows Screen-Capture Latency & Synthetic-Input Realism — Research Dump

Scope: C++ RPA tool automating real-time games on Windows.
Convention: **[VERIFIED]** = quoted from a primary source (Microsoft Learn, vendor docs, source code, vendor license) with URL. **[MEASURED]** = an actual measured number with hardware/method stated. **UNVERIFIED** = could not confirm from a primary source. **NO NUMBER FOUND** = topic searched, no measured figure exists publicly.

---

## PART A — CAPTURE APIs AND MEASURED LATENCY

### A0. Executive summary of corrections to the task's premises

| Premise in the brief | Reality | Evidence |
|---|---|---|
| dxcam README has a "DXcam vs mss vs PIL.ImageGrab" table with FPS per resolution (1920x1080 / 2560x1440 / 3840x2160) | **FALSE.** No resolution rows and no `PIL.ImageGrab` column exist in any retrievable README revision. The real table is **DXcam vs python-mss vs D3DShot**, single-row, at 240 Hz. | https://github.com/ra1nty/DXcam , https://raw.githubusercontent.com/ra1nty/DXcam/c56045dd277aa44fe8e6a54f82ebbe7732d3cd86/README.md |
| `GraphicsCaptureSession.MinUpdateInterval` was "added Windows 11 22H2" | **Docs indicate Windows 11 21H2 (build 22000), not 22H2.** Learn page unavailable in the `winrt-19041` and `winrt-20348` views, present in `winrt-22000`. The page itself has **no requirements table**, so exact contract version is UNVERIFIED. | https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.graphicscapturesession.minupdateinterval?view=winrt-20348 (redirects, `viewFallbackFrom=winrt-20348`) vs https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.graphicscapturesession?view=winrt-22000 (lists it) |
| `IDXGIOutputDuplication::GetFrameStatistics` provides `PresentCount`/`AccumulatedFrames` | **That method does not exist.** `PresentCount` lives on `IDXGIOutput::GetFrameStatistics` → `DXGI_FRAME_STATISTICS`; `AccumulatedFrames` lives on `DXGI_OUTDUPL_FRAME_INFO` returned by `AcquireNextFrame`. And `IDXGIOutput::GetFrameStatistics` is **only supported in full-screen mode**. | https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgioutput-getframestatistics , https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/ns-dxgi1_2-dxgi_outdupl_frame_info |
| NvFBC restriction is a "2018-era change" | **UNVERIFIED.** NVIDIA staff statement is **2017-11-01**; NVIDIA's own deprecation notice is a **2019-era** Windows-10 deprecation (last supported Win10 = **1803 / 17134**). Consumer-enablement patches begin at driver **440.26 (Nov 2019)**. | https://developer.nvidia.com/capture-sdk , https://forums.developer.nvidia.com/t/nvfbc-on-geforce/54460 , https://github.com/keylase/nvidia-patch |

**The single biggest architectural risk for this project:** DXGI Desktop Duplication is *documented by Microsoft to fail by design* on hybrid/discrete-GPU laptops — see A3.

---

### A1. `BitBlt` / GDI capture

**API mechanics [VERIFIED]** — Microsoft's own capture recipe is `GetDC(NULL)` → `CreateCompatibleDC` → `CreateCompatibleBitmap` → `SelectObject` → `BitBlt(SRCCOPY)` → `GetDIBits`.
https://learn.microsoft.com/en-us/windows/win32/gdi/capturing-an-image

**The `CAPTUREBLT` flag [VERIFIED, verbatim]:**
> `CAPTUREBLT` | Includes any windows that are layered on top of your window in the resulting image. By default, the image only contains your window. Note that this generally cannot be used for printing device contexts.
https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-bitblt

`CAPTUREBLT = 0x40000000`. Consequence for RPA: **without `CAPTUREBLT` you miss layered windows (i.e. the DWM-composited output of most modern apps); with it you pay a much heavier path** because the blit has to compose layered surfaces. This is exactly why `python-mss` ORs it in unconditionally (see A5).

**`BitBlt` documented hard limits [VERIFIED]:**
- *"BitBlt returns an error if the source and destination device contexts represent different devices."* → you cannot BitBlt a DC belonging to a different adapter.
- *"BitBlt only does clipping on the destination DC."*
- No colour management (`ICM: No color management is performed when blits occur.`)
Same URL as above.

**Why it fails / is slow on hardware-accelerated & exclusive-fullscreen content:**
- The GDI path reads the **DWM redirection surface**, not the GPU scanout. Microsoft documents the protection feature's dependence on composition, verbatim: *"However, it works only when the Desktop Window Manager (DWM) is composing the desktop."* — i.e. in a mode where DWM is bypassed, the same class of surface stops reflecting what is on screen.
  https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowdisplayaffinity
- WDA_EXCLUDEFROMCAPTURE (Windows 10 2004+, `0x00000011`) makes a window *invisible* to capture APIs. If a target app or overlay sets this, BitBlt silently returns the background.
  Same URL.
- Direct3D flip-model swap chains (the default for D3D11/12 apps since Windows 8) are not GDI-accessible surfaces; `GetDC(hwnd)` + `BitBlt` returns stale or blank content for such windows. **[This specific mechanism is widely reported but I did not retrieve a single Microsoft statement that says it in one sentence — treat the one-sentence phrasing as UNVERIFIED; the DWM-composition dependency above and the DDA transition clause in A3 are the verified parts.]**
- Exclusive fullscreen: DDA's own docs list *"Switch from DWM on, DWM off, or other full-screen application"* as an invalidating condition, confirming that fullscreen apps take the display out of the composed path. https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgioutputduplication-acquirenextframe

**`BitBlt` throughput — [MEASURED], third-party, Windows:**
Measured by Kyle Fu (Feb 2023), mid-tier 1920x1200 Windows laptop, 30–360 frames per run, FPS mean ± σ:

| Test | Pillow (BitBlt) | mss (BitBlt) | DXcam (DDA) | `np.sum(screen)` baseline |
|---|---|---|---|---|
| Static | 27 ± 0.5 | 29 ± 0.1 | **170 ± 3.4** | ~650 |
| Dynamic | 22 ± 1.4 | 28 ± 0.7 | **96 ± 9.2** | ~650 |
| UFO 60 FPS | ~19 | ~25 | **~60** | ~650 |
| Small Region | 27 ± 0.7 | 57 ± 0.3 | **387 ± 11.4** | ~5000 |

Author's TLDR, verbatim: *"Use DXcam. Expect ~100 FPS for mid tier PCs. … MSS is fine for cross-platform and ease of use, but will be about 3 times slower on Windows."*
https://kylefu.me/2023/02/18/python-fast-screen-capture.html

Interpretation for the RPA tool: a full-screen GDI grab on this class of machine costs **~35–45 ms per frame dynamic (~22–28 FPS)**, i.e. **~2–3 game frames of latency at 60 Hz before you have even looked at a pixel**. Region grabs help (57 FPS for a small region) but remain an order of magnitude off DDA.

---

### A2. Windows Graphics Capture (WGC) — `Windows.Graphics.Capture`

**Namespace & lifecycle [VERIFIED]:** `GraphicsCapturePicker` (or the Win32 `IGraphicsCaptureItemInterop` path) → `GraphicsCaptureItem` → `Direct3D11CaptureFramePool` → `CreateCaptureSession(item)` → `StartCapture()`; frames arrive via `FrameArrived` or `TryGetNextFrame()`; frame pool texture format must be `DXGI_FORMAT_B8G8R8A8_UNORM` for SDR (use `DXGI_FORMAT_R16G16B16A16_FLOAT` for HDR to avoid clipping).
https://learn.microsoft.com/en-us/windows/apps/develop/media-authoring-processing/screen-capture

**Version gates [VERIFIED]:**
| API | Introduced |
|---|---|
| `GraphicsCaptureSession` class | Windows 10 1803 (10.0.17134.0), UniversalApiContract **v6.0** |
| `Direct3D11CaptureFramePool.CreateFreeThreaded` | Windows 10 **1809** (10.0.17763.0), UniversalApiContract **v7.0** |
| `GraphicsCaptureSession.IsCursorCaptureEnabled` | Windows 10 **2004** (10.0.19041.0), UniversalApiContract **v10.0** |
| `GraphicsCaptureSession.IsBorderRequired` | *"Windows 10, version 2104 (introduced in 10.0.20348.0)"*, UniversalApiContract **v12.0** (docs' own wording — this is Windows Server 2022's build; the consumer-equivalent framing is Windows 11) |
| `GraphicsCaptureSession.MinUpdateInterval` | No requirements table on the page. Learn view-availability: absent at `winrt-19041` and `winrt-20348`, present at `winrt-22000` ⇒ **Windows 11 21H2 (10.0.22000)**, UniversalApiContract v13 is the indicated gate. **Exact contract version UNVERIFIED.** |
| OBS' machine-readable OS gate | `ApiInformation::IsApiContractPresent(L"Windows.Foundation.UniversalApiContract", 8)` ⇒ WGC requires **Windows 10 build 18362 (1903)** |

URLs: https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.direct3d11captureframepool.createfreethreaded , https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.graphicscapturesession.iscursorcaptureenabled , https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.graphicscapturesession.isborderrequired , https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.graphicscapturesession.minupdateinterval , https://raw.githubusercontent.com/obsproject/obs-studio/master/libobs-winrt/winrt-capture.cpp

**`CreateFreeThreaded` [VERIFIED, verbatim]:**
> Creates a frame pool where the dependency on the `DispatcherQueue` is removed and the `FrameArrived` event is raised on the frame pool's internal worker thread.
This is the correct choice for a headless C++ RPA process — no message pump / `DispatcherQueue` requirement. Introduced 1809.
https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.direct3d11captureframepool.createfreethreaded

**Cursor capture [VERIFIED]:** `IsCursorCaptureEnabled` (*"True if the cursor should be captured; otherwise, false"*), Windows 10 2004+. Note OBS deliberately **turns it off** because drawing its own cursor performs better — verbatim source comment: `/* disable cursor capture if possible since ours performs better */`.
https://raw.githubusercontent.com/obsproject/obs-studio/master/libobs-winrt/winrt-capture.cpp

**Border (yellow rectangle) — the important part [VERIFIED, verbatim from Learn]:**
> In the case of multiple simultaneous capture sessions, a yellow border is drawn around each item being captured.
> Before the system will disable the colored border around the a window or display that is being captured, your app must get consent from the user by calling `GraphicsCaptureAccess.RequestAccessAsync`, passing in the value `GraphicsCaptureAccessKind.Borderless`, which displays a prompt to the user. If the user denies access, setting this property to **false** will succeed, but the value will be ignored and the border will be displayed during subsequent captured. To call **RequestAccessAsync** with **GraphicsCaptureAccessKind.Borderless**, you must declare the **graphicsCaptureWithoutBorder** capability in your app's package manifest.
> Note that if the **IsBorderRequired** property is set to **true** for the same window or display by other apps on the device, the border will be displayed.

Practical consequences for a QuickScriptTool-style Win32 exe:
1. `graphicsCaptureWithoutBorder` is an **MSIX/package-manifest capability**. An unpackaged Win32 binary cannot declare it ⇒ **border cannot be suppressed. UNVERIFIED whether any unpackaged-app workaround exists; I found none in the docs.**
2. Suppression requires an interactive **user consent prompt** — unacceptable for a silently-started automation tool.
3. **Any other app capturing the same item can force the border back on.**
4. Windows 10 always shows the border in practice (user reports, §A7 OBS).
OBS uses the exact same call sequence (border toggle only when supported, then `session.IsBorderRequired(false)`).
URLs: https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.graphicscapturesession.isborderrequired , https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.graphicscaptureaccess.requestaccessasync

**`MinUpdateInterval` [VERIFIED it exists and its type]:**
```
public: property TimeSpan MinUpdateInterval { TimeSpan get(); void set(TimeSpan value); };
```
The Learn page documents **nothing else** — no units semantics, no default, no remarks. It is the throttle for how often the capture pipeline delivers frames (useful to cap WGC at the game's real cadence rather than burning GPU). **Default value and unit semantics are UNVERIFIED.** Windows 11 22000+ as discussed above.
https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.graphicscapturesession.minupdateinterval

**WGC frame policy (no frame statistics exposed):** WGC gives you no `AccumulatedFrames` equivalent. OBS uses a **2-buffer** `Direct3D11CaptureFramePool` (`Direct3D11CaptureFramePool::Create(device, …, 2, size)`), which is what bounds how stale a frame can be.
https://raw.githubusercontent.com/obsproject/obs-studio/master/libobs-winrt/winrt-capture.cpp

**WGC vs DDA as chosen by OBS `Automatic` [VERIFIED, source verbatim]:**
```c
if (method == METHOD_AUTO) {
    method = METHOD_DXGI;
    const int dxgi_index = gs_duplicator_get_monitor_index(monitor);
    if (dxgi_index == -1) {
        method = METHOD_WGC;
    } else {
        SYSTEM_POWER_STATUS status;
        if (GetSystemPowerStatus(&status) && status.BatteryFlag < 128) {   /* on battery */
            const uint32_t count = gs_get_adapter_count();
            if (count >= 2)                                                /* hybrid GPU */
                method = METHOD_WGC;
        }
    }
}
```
Reading: **OBS falls back to WGC exactly where DXGI DDA is unavailable or fragile — "no DXGI output index" and "hybrid laptop on battery".** That is independent engineering corroboration of A3's Microsoft KB.
https://raw.githubusercontent.com/obsproject/obs-studio/master/plugins/win-capture/duplicator-monitor-capture.c

---

### A3. DXGI Desktop Duplication API (`IDXGIOutputDuplication`)

**Core contract [VERIFIED]:**
- `AcquireNextFrame(UINT TimeoutInMilliseconds, DXGI_OUTDUPL_FRAME_INFO*, IDXGIResource**)`.
- Desktop image format is **always `DXGI_FORMAT_B8G8R8A8_UNORM`** regardless of display mode.
- Timeout `0` = poll (returns immediately), `INFINITE` = block. *"You cannot cancel the wait that you specified in the TimeoutInMilliseconds parameter"* → do not use `INFINITE` in a loop you need to be able to stop.
- Return codes: `S_OK`; `DXGI_ERROR_ACCESS_LOST`; `DXGI_ERROR_WAIT_TIMEOUT`; **`DXGI_ERROR_INVALID_CALL` if the application called AcquireNextFrame without releasing the previous frame**; `E_INVALIDARG`.
- `DXGI_ERROR_ACCESS_LOST` occurs on *"Desktop switch / Mode change / **Switch from DWM on, DWM off, or other full-screen application**"* → *"the application must release the IDXGIOutputDuplication interface and create a new IDXGIOutputDuplication for the new content."*

**Release discipline — [VERIFIED, verbatim], this is the documented latency trap:**
> The application must release the frame before it acquires the next frame. After the frame is released, the surface that contains the desktop bitmap becomes invalid; you will not be able to use the surface in a DirectX graphics operation.
>
> For performance reasons, we recommend that you release the frame just before you call the `IDXGIOutputDuplication::AcquireNextFrame` method to acquire the next frame. When the client does not own the frame, the operating system copies all desktop updates to the surface. This can result in wasted GPU cycles if the operating system updates the same region for each frame that occurs. When the client acquires the frame, the client is aware of only the final update to this region; therefore, any overlapping updates during previous frames are wasted. When the client acquires a frame, the client owns the surface; therefore, the operating system can track only the updated regions and cannot copy desktop updates to the surface. Because of this behavior, we recommend that you minimize the time between the call to release the current frame and the call to acquire the next frame.

**Therefore: holding the frame while you run detection guarantees you will see a *coalesced* image and that the OS stops copying updates into the surface. The correct pattern is CopyResource → (immediately) ReleaseFrame → then detect on your private copy.** Note DXcam deliberately violates the "release just before acquire" advice — its source comment says: *"Per Microsoft Doc … This should be called just before AquireNextFrame, but we found audio artifacts and frame pacing issue (need longer timeout for AcquireNextFrame to compensate). So DXCam default to early release."* (`dxcam/core/dxgi_duplicator.py`)
URLs: https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgioutputduplication-releaseframe , https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgioutputduplication-acquirenextframe , https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/desktop-dup-api

**Dirty/move rects [VERIFIED]:** `GetFrameDirtyRects` / `GetFrameMoveRects` / `GetFramePointerShape`. *"To reconstruct the correct desktop image, your client app must first process all the move regions and then process all the dirty regions."* `RectsCoalesced` == TRUE means the dirty rects *"might contain unmodified pixels"*. Use these for ROI updates. https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/desktop-dup-api

**Rotated displays [VERIFIED]:** *"In a rotated mode, the surface that you receive from AcquireNextFrame is always in the un-rotated orientation, and the desktop image is rotated within the surface. For example, if the desktop is set to 768x1024 at 90 degrees rotation, AcquireNextFrame returns a 1024x768 surface with the desktop image rotated within it."* Same URL.

**Mouse pointer [VERIFIED]:** the pointer is *either* composited into the desktop image *or* delivered separately via `PointerPosition` + `GetFramePointerShape`. You must check `DXGI_OUTDUPL_FRAME_INFO.PointerPosition.Visible` and draw the cursor yourself when it is not baked in. Same URL.

**Hybrid / multi-GPU failure — [VERIFIED, this is a Microsoft KB]:**
> **Cause:** This issue occurs because the DDA does not support being run against the discrete GPU on a Microsoft Hybrid system. By design, the call fails together with error code `DXGI_ERROR_UNSUPPORTED` in such a scenario.
> **Resolution:** To work around this issue, run the application on the integrated GPU instead of on the discrete GPU on a Microsoft Hybrid system.
> **More information:** When this issue occurs, the `IDXGIOutput1::DuplicateOutput` method fails and returns an error code `DXGI_ERROR_UNSUPPORTED`.

Observed error strings quoted in the KB: `Failed to create windows swapchain with 0x80070005`, `CDesktopCaptureDWM: IDXGIOutput1::DuplicateOutput failed: 0x887a0004`. (Original KB 3019314, applies to Windows 8.1; the behaviour is the documented design and is still what DXcam users hit on modern hybrid laptops.)
https://learn.microsoft.com/en-us/troubleshoot/windows-client/shell-experience/error-when-dda-capable-app-is-against-gpu

**Field evidence on modern hybrid laptops [VERIFIED as issue reports]** (from the DXcam issue tracker — the same API, so the failures transfer to any C++ implementation):
- #11 *"Win10 screenshot is black"* — `dxcam.device_info()` returns `Device[0]:<Intel(R) UHD Graphics 630>` instead of the NVIDIA 1050. https://github.com/ra1nty/DXcam/issues/11
- #8 *"Grabbing region gives COMError: -2005270527"* — `NVIDIA RTX A2000 Laptop GPU` + `Intel UHD`, `_ctypes.COMError: (-2005270527, 'The application made a call that is invalid…')` from `AcquireNextFrame`; *"Not using the region= will not throw any errors."* https://github.com/ra1nty/DXcam/issues/8
- #38 `_ctypes.COMError: (-2005270524, 'The specified device interface or feature level is not supported on this system.')` on AMD iGPU + RTX 3050 Ti Laptop. https://github.com/ra1nty/DXcam/issues/38
- #67 *"Running on a standalone graphics card will result in an error"* — works on iGPU, errors on the discrete GPU. https://github.com/ra1nty/DXcam/issues/67
- #91 `dxcam.create(device_idx=1)` → `IndexError: list index out of range`, because only adapters that own ≥1 output are enumerated. https://github.com/ra1nty/DXcam/issues/91
- #109 Secondary 4K monitor returns `None`/empty frames while the primary monitor works. https://github.com/ra1nty/DXcam/issues/109

**Recommended defensive design:** enumerate *all* DXGI adapters/outputs, attempt `DuplicateOutput` on each until one succeeds, and be prepared to recreate the duplicator on `DXGI_ERROR_ACCESS_LOST`. This is what OBS does (`static std::unordered_map<int, gs_duplicator *> instances;` refcounted per monitor, and `DXGI_ERROR_ACCESS_LOST → return false`), and what DXcam 0.3.0 added (transient HRESULT contexts `CREATE_DUPLICATION` / `FRAME_INFO` / `SYSTEM_TRANSITION`, log *"Desktop duplication access loss/system transition detected (HRESULT=0x%08X). Triggering output-change recovery."*, then `_recover_output()`).
https://raw.githubusercontent.com/obsproject/obs-studio/master/libobs-d3d11/d3d11-duplicator.cpp

**Exclusive fullscreen:** the *only* Microsoft-documented statement is the `DXGI_ERROR_ACCESS_LOST` transition clause quoted above (fullscreen apps invalidate the duplication interface). DXcam's README claims *"Stable capture for full-screen exclusive Direct3D apps"* as a capability, but its issue tracker has **zero** issues mentioning "exclusive fullscreen" (`api.github.com/search/issues?q=repo:ra1nty/DXcam+exclusive+fullscreen` → `total_count: 0`), and issue #13 records a user unable to find the advertised behaviour. **Treat "DDA captures exclusive fullscreen reliably" as a vendor capability claim, not a verified guarantee.**
https://github.com/ra1nty/DXcam/issues/13

---

### A4. Python `dxcam` (https://github.com/ra1nty/DXcam) — README claims

Current release **0.3.0** (2026-03-12); wheels for CPython 3.10–3.14.
https://pypi.org/project/dxcam/

**The only benchmark tables that exist [MEASURED — author's own machine: 5900X + RTX 3090, 240 Hz output, Blur Busters UFO test at 240 fps, 5 runs, light-moderate background load]**

> When using a similar logic (only capture newly rendered frames) running on a 240fps output, DXCam, python-mss, D3DShot benchmarked as follow:

| | DXcam | python-mss | D3DShot |
|---|---|---|---|
| Average FPS | **239.19** | 75.87 | 118.36 |
| Std Dev | 1.25 | 0.5447 | 0.3224 |

> For Targeting FPS:

| (Target) \ (mean, std) | DXcam | python-mss | D3DShot |
|---|---|---|---|
| 60 fps | 61.71, 0.26 | N/A | 47.11, 1.33 |
| 30 fps | 30.08, 0.02 | N/A | 21.24, 0.17 |

**Caveat the author himself publishes [verbatim]:** *"You will see some benchmarks online claiming 1000+fps capture while most of them is busy-spinning a for loop on a staled frame (no new frame rendered on screen in test scenario)."*

An intermediate README revision has **238.79** instead of 239.19 for the same table:
https://raw.githubusercontent.com/ra1nty/DXcam/c56045dd277aa44fe8e6a54f82ebbe7732d3cd86/README.md
The original 2022 README has **no numbers at all**, only the benchmark loop code:
https://raw.githubusercontent.com/ra1nty/DXcam/46dfcbfe7aed2f00f38176c196c2a9e34249b545/README.md

**Headline (not a measured table):** *"Higher capture throughput (240+fps on 1080p)"*.

**Third-party measured number, contributor PR (never merged, `"merged_at":null`) [MEASURED]:** `grab()` of a 1440x2560 screen — *"profiling total time spent in `ctypes.string_at()` went from ~20% to ~0%. … Overall FPS improvements on my machine, grabbing 1440x2560 in BGRA ~271FPS -> ~685FPS"*. https://github.com/ra1nty/DXcam/pull/62

**`output_idx` / `device_idx` [VERIFIED]:**
- README: *"Each output (monitor) is associated with one DXCamera instance."* / `camera = dxcam.create()  # primary output on device 0`
- `create()` docstring: `device_idx: DXGI adapter index.` / `output_idx: Output index on the selected adapter. None chooses the primary output.`
- **`dxcam.create()` is a singleton per `(device_idx, output_idx, backend)`** — a second call logs *"DXCamera instance already exists for device=%s output=%s backend=%s; returning existing instance. Delete the old object with `del obj` to recreate it with new parameters."*
- `dxcam.device_info()` / `dxcam.output_info()` print the adapter list, e.g. `'Device[0]:<Device Name:NVIDIA GeForce RTX 3090 Dedicated VRAM:24348Mb VendorId:4318>\n'`, `'Device[0] Output[0]: Res:(1920, 1080) Rot:0 Primary:True\n'`
https://raw.githubusercontent.com/ra1nty/DXcam/master/dxcam/__init__.py

**`region` [VERIFIED]:** tuple `(left, top, right, bottom)` in output coordinates; the returned array is `(H, W, 3)`; validated against output size (`"Invalid Region: Region should be in {width}x{height}"`). **Constraint:** *"grab(region=...) is not supported while capture is running. Use start(region=...) to configure capture region."*
https://raw.githubusercontent.com/ra1nty/DXcam/master/dxcam/types.py

**`target_fps` [VERIFIED]:** default 60; `0` disables timer pacing. README: *"DXcam uses high-resolution pacing with drift correction to run near target_fps. … On Python 3.11+, DXcam relies on Windows high-resolution timer behavior used by `time.sleep()`. On older versions, DXcam uses WinAPI waitable timers directly."* Historical guidance: *"Should not be made greater than 160."*
The 2022 README states the mechanism and the measured claim explicitly [verbatim]:
> To make DXCamera capture close to the user specified target_fps, we used the undocumented `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` flag to create a Windows Waitable Timer Object. This is far more accurate (**+/- 1ms**) than Python (<3.11) `time.sleep` (**min resolution 16ms**). The implementation is done through ctypes creating a perodic timer.

**`processor_backend` [VERIFIED]:** `Literal["cv2", "numpy"]`. *"DXcam capture backends (dxgi/winrt) first acquire a BGRA frame. The processor backend then handles post-processing: optional rotation/cropping preparation, color conversion to your output_color."* `BGRA` is the leanest path (no OpenCV needed); `RGB`/`BGR`/`RGBA`/`GRAY` require conversion.

**`grab()` vs `get_latest_frame()` — the semantics matter a lot for frame freshness [VERIFIED, verbatim]:**
> `grab()` returns a `numpy.ndarray`. **`None` if no new frame is available since the last capture** (for backward compatibility); use `camera.grab(new_frame_only=False)` to make dxcam always return the latest frame.
> `frame = camera.get_latest_frame()  # blocks until a frame is available`
> The screen capture mode spins up a thread that polls newly rendered frames and stores them in an **in-memory ring buffer**. … When `start()` capture is running, calling `grab()` reads from the in-memory ring buffer instead of directly polling DXGI.

Original 2022 wording: *"`.grab` will return `None` if there is no new frame since the last time you called `.grab`. Usually it means there's nothing new to render since last time (E.g. You are idling)."* / *"`.get_latest_frame` by default will block until there is a new frame available since the last call … To change this behavior, use `video_mode=True`."* / *"`get_latest_frame` … assume the user to process frames in a LIFO pattern. This is a read-only action and won't pop the processed frame from the buffer."*

**Mechanism:** `AcquireNextFrame(0, …)`; on `DXGI_ERROR_WAIT_TIMEOUT` it sets `self.updated = False`, so no new frame ⇒ `grab()` returns `None`. Ring buffer `max_buffer_len` **default 8** in current code (the 2022 README said 64; the default changed). `max_buffer_len=1` is clamped to 2.
https://raw.githubusercontent.com/ra1nty/DXcam/master/dxcam/core/dxgi_duplicator.py

**Frame timestamp [VERIFIED]:** `camera.get_latest_frame(with_timestamp=True) -> (frame, frame_timestamp)`. *"For `backend="dxgi"`, this value comes from `DXGI_OUTDUPL_FRAME_INFO.LastPresentTime`. For `backend="winrt"`, this value is derived from WinRT `SystemRelativeTime`."*

**Backends [VERIFIED]:** `backend="dxgi"` (default, Desktop Duplication) / `backend="winrt"` (WGC, needs `pip install "dxcam[winrt]"`). README guideline: *"If you need cursor rendering, use `winrt`."* / *"Start with `dxgi` for most workloads, especially one-shot grab."* The WinRT path exposes WGC's toggles via env vars `DXCAM_WINRT_BORDER_REQUIRED`, `DXCAM_WINRT_CURSOR_CAPTURE`, `DXCAM_WINRT_DIRTY_REGION_MODE`; default frame pool size **2**, default frame wait 0.002 s.
https://raw.githubusercontent.com/ra1nty/DXcam/master/dxcam/core/winrt_duplicator.py

**dxcam's documented limitations: essentially absent from the README.** There is no limitations/requirements/known-issues section; the README only asserts capabilities. The 2022 README's multi-GPU note was explicitly hedged: *"(cross GPU untested, but I hope it works.)"*. The real limitations live in Microsoft's DDA docs (A3) and in the issue tracker (#11, #8, #38, #67, #91, #109).

---

### A5. Python `mss` (https://github.com/BoboTiG/python-mss)

**Windows implementation = GDI `BitBlt` + `CAPTUREBLT` — [VERIFIED from source]:**
```python
gdi.BitBlt(memdc, 0, 0, width, height, srcdc, region.left, region.top, SRCCOPY | CAPTUREBLT)
```
with `CAPTUREBLT = 0x40000000`, `SRCCOPY = 0x00CC0020`, source DC = `user32.GetWindowDC(0)`, followed by `gdi.GdiFlush()` (*"This ensures DIB memory is fully updated before reading."*). Module docstring: *"GDI-based backend for MSS on Microsoft Windows. … This implementation uses CreateDIBSection for direct memory access to pixel data."*
The 10.2.0 change replaced `GetDIBits` with `CreateDIBSection`: *"This reduces memory overhead and improves reliability during long capture sessions."*
https://raw.githubusercontent.com/BoboTiG/python-mss/main/src/mss/windows/gdi.py
Pre-restructure equivalent (v10.1.0): https://raw.githubusercontent.com/BoboTiG/python-mss/v10.1.0/src/mss/windows.py

**The GDI type cost is not published for Windows.** The project's only measured numbers are **Linux/X11**:
> In local testing (local desktop system, Debian testing, X11, 4K display), a tight loop capturing the full screen (1000 iterations, best of three runs) improved from: 10.1.0: **46.2 ms** per screenshot → 10.2.0: **9.48 ms** per screenshot. This represents roughly a 5× reduction in capture time in that environment.
and, for the Linux backends: *"`xshmgetimage` (default) — The fastest backend, based on `xcb_shm_get_image()`. It is roughly three times faster than `xgetimage`."*
https://python-mss.readthedocs.io/stable/release-history/v10.2.0.html , https://python-mss.readthedocs.io/stable/usage.html

**Why mss is slower than DDA on Windows — verified reasons, not marketing:**
1. It goes through the **GDI/USER path** (`GetWindowDC(0)` → `BitBlt` with `CAPTUREBLT` → `GdiFlush` → CPU buffer). `CAPTUREBLT` forces composition of layered surfaces on every grab (A1).
2. There is **no dirty-rect or per-output duplication**: every `grab()` is a full DC blit of the requested region.
3. **No cursor capture on Windows** — source: `def cursor(self) -> None: """Retrieve all cursor data. Pixels have to be RGB.""" return`. The docs confirm `with_cursor` is *"(GNU/Linux only)"*.
4. **No direct-buffer path on Windows** — *"Requirements: Python 3.12 or later / GNU/Linux. Support for additional operating systems is planned."*
5. The maintainer notes: *"We plan to add a DXGI backend in the near future."* (10.2.0 notes) — i.e. it does not exist yet.
6. Backends doc: *"Windows also exposes the named `gdi` backend, which is currently the same as `default`."*
https://python-mss.readthedocs.io/stable/api.html , https://raw.githubusercontent.com/BoboTiG/python-mss/main/docs/source/usage.rst

**Perf-adjacent docs worth copying into your own tool [VERIFIED]:**
- DPI: `_set_dpi_awareness()` calls `ctypes.windll.shcore.SetProcessDpiAwareness(2)` (**PROCESS_PER_MONITOR_DPI_AWARE**) on Windows 8.1+, else `SetProcessDPIAware()`. Without this your captured bitmap is DPI-virtualized and your click coordinates are wrong on scaled displays.
- *"Calls to `mss.MSS.grab()` (and other capture methods) are serialized automatically, meaning only one thread will capture at a time."*
- Creating a fresh `MSS()` per screenshot is documented as *"a bad usage"*.
- README: *"In case of scaling and high DPI issues for external monitors: some packages (e.g. `mouseinfo` / `pyautogui` / `pyscreeze`) incorrectly call `SetProcessDpiAware()` during import process. To prevent that, import `mss` first."*
- Resource handling: *"Device contexts (srcdc / memdc) are acquired and released within each call. This avoids holding GDI resources across threads and allows `MSS()` construction to succeed even when `GetWindowDC(0)` would fail (locked screen, UAC, RDP). See issue #509."*

**Pillow `ImageGrab` for comparison [VERIFIED from C source]** — same GDI path, and it exposes `CAPTUREBLT` as the `include_layered_windows` Python argument (default **False**):
```c
rop = SRCCOPY;
if (includeLayeredWindows) {
    rop |= CAPTUREBLT;
}
if (!BitBlt(screen_copy, 0, 0, width, height, screen, x, y, rop)) { goto error; }
```
Source DC: `CreateDC("DISPLAY", NULL, NULL, NULL)`. It also sets per-monitor DPI awareness via `SetThreadDpiAwarenessContext((HANDLE)-3)` (DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE).
**Note the default: `ImageGrab.grab()` does NOT capture layered windows unless you pass `include_layered_windows=True`.** That is a real correctness bug source for RPA against modern apps.
https://raw.githubusercontent.com/python-pillow/Pillow/main/src/display.c

---

### A6. NVIDIA NvFBC / AMD — vendor capture SDKs

**NVIDIA Capture SDK (NvFBC/NVIF) — Windows status [VERIFIED, verbatim from NVIDIA]:**
> **Important Notice for Windows developers**
> - NVFBC has been deprecated on Windows 10 and above for reasons explained in [this document]. **Last supported Windows 10 version is 1803, build 17134**
> - Windows 10 provides native capture APIs that can be considered as alternatives to NVFBC.
https://developer.nvidia.com/capture-sdk (deprecation bulletin: https://developer.nvidia.com/designworks/capture_sdk/docs/nvfbc_win10_deprecation_tech_bulletin002.pdf)

**License gate [VERIFIED, verbatim §1.1]:**
> (i) install, use and reproduce the software delivered by NVIDIA … **provided that the software is executed only in NVIDIA GRID, Tesla or Quadro 2000+ hardware products** …
> (ii) sub-license and distribute in binary format the software … **for use by your recipients only in NVIDIA GRID, Tesla or Quadro 2000+ hardware products** …

**And §2.1(x) — this is why no NvFBC latency number exists publicly [verbatim]:**
> (x) **disclose the results of any benchmarking or other competitive analysis relating to the Licensed Software without the prior written permission from NVIDIA**;
https://developer.nvidia.com/capture-sdk-software-license-agreement

**Timeline evidence [VERIFIED]:**
- NVIDIA staff, 2017-11-01: *"NVFBC is a licensed product and not supported on GeForce for all users."* https://forums.developer.nvidia.com/t/nvfbc-on-geforce/54460
- Older restriction threads (2014/2016) prove it predates 2018: https://forums.developer.nvidia.com/t/enabling-nvfbc-on-geforce/161867 , https://forums.developer.nvidia.com/t/nvfbc-error-unsupported-platform-in-capture-sdk/162352
- The consumer-enablement patch project's version table shows NVFBC patch = **NO for every driver before 440.26** (first YES: 440.26, Nov 2019). https://raw.githubusercontent.com/keylase/nvidia-patch/master/README.md
- 2026 open thread, still unanswered by NVIDIA: https://forums.developer.nvidia.com/t/commercial-licensing-question-nvfbc-capture-sdk-access-for-geforce-screen-recording-software/373093

**OBS and NvFBC [VERIFIED]:** OBS has never shipped an official NvFBC source. The only one is a third-party **Linux-only** plugin, verbatim: *"IMPORTANT: This plugin will NOT work with your regular NVIDIA GeForce graphics cards! … Quadro Desktop | Quadro Mobile | Tesla"*, and it is disabled on OBS ≥28 (GLX removal). Plugin author, verbatim: *"The nvfbc API has been deprecated in Windows. One should use the desktop duplication API from windows instead."* A community OBS 30 fork requires a patched driver: *"Requirements: Patched NVIDIA driver with NvFBC enabled."*
https://obsproject.com/forum/resources/obs-nvfbc.796/ , https://obsproject.com/forum/threads/obs-nvfbc.105590 , https://raw.githubusercontent.com/PancakeTAS/obs-nvfbc-v30/master/README.md

**Conclusion: NvFBC is a dead end for a consumer Windows RPA tool.** Not merely license-gated — deprecated on Windows 10+ since 2019, and benchmark publication is contractually forbidden.

**AMD:**
- **AMF (Advanced Media Framework)** is primarily an *encoder* framework, and its capture component **is itself a DXGI Desktop Duplication wrapper** — [VERIFIED from the MIT-licensed public header `DisplayCapture.h`]:
  `// Desktop duplication interface declaration` / `AMFCreateComponentDisplayCapture(...)`, `#define AMFDisplayCapture L"AMFDisplayCapture"`, `AMF_DISPLAYCAPTURE_MODE_KEEP_FRAMERATE = 0`, `AMF_DISPLAYCAPTURE_MODE_WAIT_FOR_PRESENT = 1`, `AMF_DISPLAYCAPTURE_MODE_GET_CURRENT_SURFACE = 2`, `MonitorIndex`, `FrameRate` (*"if 0 - capture rate will be driven by flip event from fullscreen app or DWM"*), `DuplicateOutput`, `EnableDirtyRects`, `Rotation`, and output props `DirtyRects`, `FrameIndex`, `FlipTimesamp`.
  **So AMF gives you nothing DDA does not already give you.**
  https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/AMF/master/amf/public/include/components/DisplayCapture.h
- **ADL (AMD Display Library)** is *"used strictly for exposing 'Graphics Hardware' support"* — display/GPU monitoring and control (Overdrive8, PMLog, Wattman, I2C-via-SMU, PowerXpress). **No frame-capture API found.** https://gpuopen.com/archived/adl/ , https://gpuopen-librariesandsdks.github.io/adl/
- **A license-gated AMD capture API analogous to NvFBC: NOT FOUND.** State this as *absence of evidence*, not proof of absence.

---

### A7. OBS — DXGI (Display Capture) vs WGC, and what OBS documents about latency

**Naming changed over time [VERIFIED from locale files]:**
- OBS 27.2.0: `Method.DXGI="DXGI Desktop Duplication"`, `Method.WindowsGraphicsCapture="Windows 10 (1903 and up)"`, `WindowCapture.Method.BitBlt="BitBlt (Windows 7 and up)"`
- Current master: `Method.DXGI="Legacy (DXGI)"`, `Method.WindowsGraphicsCapture="Modern (WGC)"`, `WindowCapture.Method.BitBlt="Legacy (BitBlt)"`, `ForceSdr="Force SDR"`, `Compatibility.GameCapture.WrongGPU="If the preview is blank, make sure %name% is running on the same GPU as OBS."`
https://raw.githubusercontent.com/obsproject/obs-studio/27.2.0/plugins/win-capture/data/locale/en-US.ini , https://raw.githubusercontent.com/obsproject/obs-studio/master/plugins/win-capture/data/locale/en-US.ini

**Automatic selection logic:** see A2 (DXGI by default; WGC when no DXGI output index exists, or on battery with ≥2 adapters).

**Documented latency/buffering numbers: NO MEASURED NUMBER FOUND in any OBS primary source.** No OBS KB page, release note, or source comment states a capture-latency figure in ms or frames. What *is* verifiable from source:
- DXGI path polls with **`AcquireNextFrame(0)`** (zero timeout — take whatever is newest, never block), `CopyResource`, immediate `ReleaseFrame()`.
- WGC path uses a **2-frame** `Direct3D11CaptureFramePool` + `FrameArrived`.
- Neither path exposes a user-facing capture buffer/queue setting.
- Teardown comment in `duplicator-monitor-capture.c`, verbatim: *"completely shut down monitor capture if not in use, otherwise it can sometimes generate system lag when a game is in fullscreen mode"*, with `#define RESET_INTERVAL_SEC 3.0f` — a mode change can blank the source for up to ~3 s.
- **One Display Capture per display** [VERIFIED, OBS KB]: *"You can only add one Display Capture source per display. If you need your display in multiple scenes, make sure to add references to the existing Display Capture Source rather than created a new source."* (This mirrors the underlying DDA one-duplication-per-output limit.) https://obsproject.com/kb/display-capture-sources
- OBS KB explicitly recommends Game Capture over Display Capture: *"This source directly captures the DirectX or OpenGL game you are playing. Game Capture is the most efficient … and should always be tried first."* https://obsproject.com/kb/game-capture-source
- WGC yellow border on Windows 10, user-reported with repro (OBS 32.1.1, Win10 22H2 19045.6466, RTX 5090), verbatim: *"Manually selecting 'Windows 10 (1903 and up)': consistently captures Frame Generation correctly, but **adds the yellow Windows capture border**."* and *"Automatic now always selects DXGI Desktop Duplication instead. DXGI captures only the original rendered frames before NVIDIA Frame Generation, so recordings and live streams appear much less smooth than what is displayed on the monitor."* https://github.com/obsproject/obs-studio/issues/13719
- **Key architectural takeaway for the RPA tool:** OBS' display-capture latency is bounded by "poll at zero timeout and take the newest frame, release immediately". There is no magic buffer to tune — the freshness problem is solved by *not holding the frame*, which is exactly what Microsoft's `ReleaseFrame` remark prescribes.

---

### A8. GPU→CPU readback cost

**`ID3D11DeviceContext::Map` [VERIFIED, verbatim]:**
> Gets a pointer to the data contained in a subresource, **and denies the GPU access to that subresource.**
> This method also returns **`DXGI_ERROR_WAS_STILL_DRAWING`** if MapFlags specifies **`D3D11_MAP_FLAG_DO_NOT_WAIT`** and the GPU is not yet finished with the resource.
> **Don't read from a subresource mapped for writing** … you must ensure that your app does not read the subresource data to which the pData member … points because doing so can cause a significant performance penalty. The memory region to which pData points can be allocated with **PAGE_WRITECOMBINE**, and your app must honor all restrictions that are associated with such memory.
> Even the following C++ code can read from memory and trigger the performance penalty … `*((int*)MappedResource.pData) = 0;` … Use the appropriate optimization settings and language constructs to help avoid this performance penalty.
https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-map

Practical reading:
- `D3D11_MAP_FLAG_DO_NOT_WAIT` is the **non-blocking** readback primitive: on `DXGI_ERROR_WAS_STILL_DRAWING` you must retry next tick rather than stall. This is the correct way to avoid a full GPU pipeline flush in an RPA loop — but it means the frame you finally get is one you *already* queued, increasing age (see A9).
- `PAGE_WRITECOMBINE` means **write-combined, uncached** memory: reading it with normal cached-load patterns is catastrophically slow; the standard fixes are (a) `memcpy` row-by-row into a cached staging buffer, and (b) never read individual bytes.

**The canonical pipeline [VERIFIED pattern]:** default (GPU-local) texture → `CopyResource`/`CopySubresourceRegion` into a `D3D11_USAGE_STAGING` texture with `CPU_ACCESS_READ` → `Map(..., D3D11_MAP_READ, [D3D11_MAP_FLAG_DO_NOT_WAIT], ...)` → read → `Unmap`.
Usage/CPU-access matrix: https://learn.microsoft.com/en-us/windows/win32/direct3d11/d3d11-usages
`CopySubresourceRegion` (ROI copy): https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-copysubresourceregion

**Measured full-HD RGBA readback numbers: NO RELIABLE MEASURED NUMBER FOUND.** I could not retrieve a primary source that publishes a concrete ms figure for a 1920x1080 RGBA staging readback on named hardware. Searches surfaced only: an unretrieved Stack Overflow question (`ID3D11DeviceContext::Map slow performance`, https://stackoverflow.com/questions/40808759/id3d11devicecontextmap-slow-performance — Cloudflare-blocked, **not verified**), an academic thesis on D3D12 heap copy times (http://bth.diva-portal.org/smash/record.jsf?pid=diva2%3A1454508 — not retrieved), and the `D3D11_USAGE_STAGING` memory-type discussion (https://stackoverflow.com/questions/50396189 — not retrieved). **Do not cite a number here from this research.**

**First-principles bound you can compute yourself (arithmetic, not a measurement):** 1920×1080×4 B = **8.294 MB** per full-frame readback. The PCIe transfer itself is trivial at ×16 Gen3/Gen4 bandwidth; the real cost is the **pipeline sync/stall** — the GPU must finish all queued work before the staging copy is coherent. That is why the techniques below matter far more than bandwidth.

**Techniques to reduce readback cost (all API-verified even where numbers are not):**
1. **Downscale on GPU before readback.** Render/blit the ROI into a small texture (e.g. 640×640 = 1.6 MB) and copy *that* to staging. Cuts bytes ~5× and, more importantly, cuts the CPU-side per-pixel work. Template matching almost never needs native resolution.
2. **Copy only the ROI with `CopySubresourceRegion`** straight from the acquired duplication surface into the staging texture — never `CopyResource` the full frame then crop on the CPU.
3. **Read only a dirty rect** — `IDXGIOutputDuplication::GetFrameDirtyRects` (A3) tells you what actually changed. Note `RectsCoalesced` warns the rects may include unmodified pixels.
4. **Double/triple-buffer the staging textures** so you map the texture filled N frames ago while the GPU fills the current one, and use `D3D11_MAP_FLAG_DO_NOT_WAIT` to never block.
5. **Keep the cursor out of the readback** when you don't need it (`IsCursorCaptureEnabled=false` in WGC; skip pointer compositing in DDA) — it is pure extra CPU/GPU work per frame.
6. **Never do CPU colour conversion in an inner loop**: capture BGRA and, if you need RGB, either do it on the GPU or accept BGRA (OpenCV's `COLOR_BGRA2BGR` is vectorized; manual per-pixel Python/naive C++ is the classic killer — cf. dxcam's `processor_backend` and the ~20%-of-time `ctypes.string_at()` finding in PR #62).

---

### A9. Frame freshness / staleness — how to know the frame is "now"

**Available clocks, all QPC-based [VERIFIED]:**

| Source | Field | Documented meaning |
|---|---|---|
| DXGI DDA | `DXGI_OUTDUPL_FRAME_INFO.LastPresentTime` | *"The time stamp of the last update of the desktop image. The operating system calls the `QueryPerformanceCounter` function to obtain the value. A zero value indicates that the desktop image was not updated since an application last called AcquireNextFrame."* |
| DXGI DDA | `DXGI_OUTDUPL_FRAME_INFO.LastMouseUpdateTime` | Same clock; *"A zero value indicates that the position or shape of the mouse was not updated since…"* |
| DXGI DDA | `DXGI_OUTDUPL_FRAME_INFO.AccumulatedFrames` | *"The number of frames that the operating system accumulated in the desktop image surface since the calling application processed the last desktop image."* **`== 1` means you kept up; `> 1` means you missed (n−1) frames.** |
| DXGI DDA | `RectsCoalesced` | TRUE ⇒ dirty regions were coalesced and *"might contain unmodified pixels"* |
| WGC | `Direct3D11CaptureFrame.SystemRelativeTime` | *"The QPC (QueryPerformanceCounter) time at which the compositor rendered the frame."* |
| DWM | `DWM_TIMING_INFO.qpcCompose` / `cFrame` / `cRefreshFrame` / `qpcVBlank` / `cFramesLate` / `cFramesMissed` / `cFramesDropped` / `cFramesPending` / `cFramesOutstanding` | Composition timing |
| DXGI output | `DXGI_FRAME_STATISTICS.SyncQPCTime` | *"the same as the value returned by the QueryPerformanceCounter function"*; `SyncGPUTime` is *"Reserved. Always returns 0."* |

URLs: https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/ns-dxgi1_2-dxgi_outdupl_frame_info , https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.direct3d11captureframe.systemrelativetime , https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/ns-dwmapi-dwm_timing_info , https://learn.microsoft.com/en-us/windows/win32/api/dxgi/ns-dxgi-dxgi_frame_statistics

**Converting to "age of frame in ms" [VERIFIED the clocks; the formula is arithmetic]:**
```
LARGE_INTEGER now, freq;  QueryPerformanceCounter(&now);  QueryPerformanceFrequency(&freq);
double age_ms = (double)(now.QuadPart - frameQpc.QuadPart) * 1000.0 / (double)freq.QuadPart;
```
- DDA: `frameQpc = FrameInfo.LastPresentTime`. **Guard `LastPresentTime == 0`** — that means *no new desktop image*, so the correct action is "do nothing this tick", not "compute a huge age".
- DDA: also reject/flag when `FrameInfo.AccumulatedFrames > 1`.
- WGC: `frameQpc = frame.SystemRelativeTime` (a WinRT `TimeSpan`; its `Duration` field is already in QPC units — verify at runtime against `QueryPerformanceFrequency` rather than assuming 100 ns ticks).
- `DwmGetCompositionTimingInfo` is usable for DWM-level sanity (`qpcCompose`, `cFramesLate`, `cFramesMissed`) but **on Windows 8.1+ `hwnd` must be `NULL`** or the call returns `E_INVALIDARG`. It is a **global** reading, not per-window, so it cannot tell you the age of *your* target window's content.
  https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/nf-dwmapi-dwmgetcompositiontiminginfo

**What a "60 Hz game + DWM composition" costs — careful accounting:**
- **Verified, mechanical components:** one display refresh period (16.67 ms at 60 Hz) is the quantization floor for *any* capture path, because the pixel content cannot change faster than the scanout. DDA's `LastPresentTime` is stamped at present time, so it already excludes the wait for the next vblank. WGC's `SystemRelativeTime` is stamped at *compositor render* time, which is **after** the app presented and **before** the frame is scanned out — so a WGC frame's `age` understates true end-to-end display lateness by up to one refresh period.
- **The quantified "DWM adds X ms" figure: NO MEASURED NUMBER FOUND in a primary source.** Blur Busters forum threads discuss DWM input lag (https://forums.blurbusters.com/viewtopic.php?p=97245) but I did not retrieve a measurement table from them, and I will not invent a number. Treat any specific "DWM adds 1 frame / 2 frames" claim as **UNVERIFIED**.
- **What you can state with confidence:** with a 60 Hz target and a correctly-written DDA loop (zero-timeout poll → CopyResource → immediate ReleaseFrame → detect on your copy), the frame you act on is **1–2 refresh periods old (≈17–33 ms)** by the time you finish detection, plus your own detection time. Any *additional* full frame of buffering inside your tool (e.g. a "latest frame" queue you drain lazily, or holding the duplication frame across detection) adds another 16.67 ms each.
- **Design rule:** compute `age_ms` on every frame; if `age_ms` exceeds ~1.5 refresh periods, mark the frame stale and **skip the action** rather than clicking on obsolete pixels. Also alarm on `AccumulatedFrames > 1`. These two checks are the only *verified* freshness signals Windows gives you.

---

### A10. Measured end-to-end "capture → detect → click" latency from published projects

**What exists [MEASURED, qualitative/aggregate]:**
- **OSWorld-Human** (WukLab, arXiv 2506.16042) — the only rigorous public end-to-end computer-use-agent latency study I found. Verbatim abstract: *"Computer-use agents (CUAs) are often unusable due to extreme end-to-end latency—taking **tens of minutes** for tasks humans complete in just a few. We present the first temporal performance study of computer-use agents on OSWorld and find that **large model calls for planning and reflection dominate latency**, with later steps taking up to **3× longer** than earlier ones. … Evaluating 16 agents, we find even top performers take **1.4–2.7× more steps** than necessary."*
  https://arxiv.org/abs/2506.16042 , https://raw.githubusercontent.com/WukLab/osworld-human/main/README.md , blog: https://mlsys.wuklab.io/posts/oshuman/
  **Takeaway:** for LLM-driven agents the VLM/LLM call dominates by orders of magnitude; the capture/input path is *not* the bottleneck. That is the opposite of a game-RPA tool, where capture+detect is the whole budget.
- **dxcam's own numbers** (§A4) are the closest thing to a published capture-stage measurement for game pipelines: **239.19 FPS average (≈4.2 ms/frame)** on a 5900X + RTX 3090 at 240 Hz, and **61.71 FPS ±0.26 when targeting 60**. mss/managed GDI sits at **~76 FPS (≈13 ms/frame)**, and the independent Windows-laptop measurement in A1 puts GDI at **~22–28 FPS (≈35–45 ms/frame)** under dynamic content.

**Per-stage "capture→detect→click" numbers from aimbot/RPA projects: NO RELIABLE PUBLISHED MEASUREMENT FOUND.** Projects that do this (shaarmander/trt-aimbot, hashk014/Universal-AI-aimbot) publish architecture but not latency breakdowns. `trt-aimbot` does publish verified *practical* findings worth acting on, verbatim: *"Make sure the game is running in **windowed mode, not fullscreen**"*, *"Try running the aimbot with administrator privileges"*, *"Some games with anti-cheat may block screen capture tools"*, and it captures *"a 640x640 square from the center of the window"* via dxcam — i.e. the standard ROI-based design.
https://github.com/shaarmander/trt-aimbot , https://github.com/hashk014/Universal-AI-aimbot

**Recommended budget model for the C++ tool (assembled from the verified components; the total is an engineering estimate, not a measurement):**
```
t_total ≈ age_of_frame (0 in the best case, ≤16.7 ms typical)
        + readback + preprocess (ROI only; single-digit ms if GPU-downscaled)
        + detect (template match on ROI, typically <1 ms for small ROIs; model inference dominates if used)
        + SendInput latency + game input→photon path (≥1 frame; not measurable in-process)
```
Instrument `age_ms` (A9) at the point of *decision*, not at capture — that is the number that tells you whether the click will land on the pixels you saw.

---

## PART B — SYNTHETIC INPUT REALISM

### B1. `SendInput` vs legacy `mouse_event`/`keybd_event`

**`SendInput` documented behaviour [VERIFIED, verbatim]:**
> The function returns the number of events that it successfully inserted into the keyboard or mouse input stream. If the function returns zero, the input was already blocked by another thread.
> **This function fails when it is blocked by UIPI.** Note that neither `GetLastError` nor the return value will indicate the failure was caused by UIPI blocking.
> This function is **subject to UIPI. Applications are permitted to inject input only into applications that are at an equal or lesser integrity level.**
> The **SendInput** function inserts the events in the INPUT structures **serially** into the keyboard or mouse input stream. **These events are not interspersed with other keyboard or mouse input events inserted either by the user (with the keyboard or mouse) or by calls to keybd_event, mouse_event, or other calls to SendInput.**
> This function **does not reset the keyboard's current state.** Any keys that are already pressed when the function is called might interfere with the events that this function generates. To avoid this problem, check the keyboard's state with the `GetAsyncKeyState` function and correct as necessary.
> An accessibility application can use **SendInput** to inject keystrokes corresponding to application launch shortcut keys that are handled by the shell. **This functionality is not guaranteed to work for other types of applications.**
https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-sendinput

Key design consequences:
1. **"Serially … not interspersed"** is the documented reason injected input is atomic w.r.t. real input: a batch of `INPUT` structs goes in as one unit. This is a *reliability* benefit (no real click sneaks between your move and your click) but also means a large batch delays real user input.
2. **The `GetAsyncKeyState` warning is a real bug class** for a macro tool: if the user is physically holding a key when you start, your synthetic state desynchronizes. Poll and clear before injecting.
3. **UIPI blocks elevation mismatch silently** — `SendInput` returns 0 with no useful `GetLastError`. If your target runs elevated (many games with anti-cheat do), a non-elevated injector simply gets `0` back. Workarounds: run elevated, or ship a `uiAccess="true"` manifest with a signature-valid binary installed under `Program Files` (**UNVERIFIED as an exact recipe in this research — the UIPI rule itself is verified; the uiAccess escape hatch is documented separately by Microsoft and I did not retrieve that page**).
4. **Verified by Raymond Chen, "Replaying input is not the same as reprocessing it" (2012-12-06):** replaying received input through `SendInput` gives every other input observer *"a second copy of your replayed events"* — *"If it were a keyboard event you replayed, a keyboard hook (or any code which subclassed your window) would see a key go down twice."* And it can reorder events, producing a stuck mouse button. **Direct relevance: if your RPA tool also installs a low-level hook to record the user, any `SendInput` injection will appear in that hook's stream.**
   https://devblogs.microsoft.com/oldnewthing/20121206-00/?p=5903

**Where `SendInput` actually sits in the stack — Raymond Chen, four posts [VERIFIED, verbatim]. This is the mechanism behind the "serially / not interspersed" doc wording:**

1. *"How do I simulate input without SendInput?"* (2010-12-21):
   > **SendInput operates at the bottom level of the input stack. It is just a backdoor into the same input mechanism that the keyboard and mouse drivers use to tell the window manager that the user has generated input.** The SendInput function doesn't know what will happen to the input. That is handled by much higher levels of the window manager, like the components which hit-test mouse input to see which window the message should initially be delivered to.
   https://devblogs.microsoft.com/oldnewthing/20101221-00/?p=11953
2. *"When something gets added to a queue, it takes time for it to come out the front of the queue"* (2014-02-13) — **the canonical explanation of why `assert(GetAsyncKeyState('A') < 0)` right after `SendInput` can fail:**
   > **When you call SendInput, you're putting input packets into the system hardware input queue.** (Note: Not the official term. That's just what I'm calling it today.) **This is the same input queue that the hardware device driver stack uses when physical devices report events.** The message goes into the hardware input queue, where the **Raw Input Thread** picks them up. The Raw Input Thread runs at high priority… If there are low-level input hooks, it has to call each of those hooks to see if any of them want to reject the input… **Only after all the low-level hooks sign off on the input is the Raw Input Thread allowed to modify the input state and cause GetAsyncKeyState to report that the key is down.**
   https://devblogs.microsoft.com/oldnewthing/20140213-00/?p=1773
3. *"The stack of messages"* (2016-07-15): the diagram labels are *"SendMessage inserts messages here"* / *"PostMessage inserts messages here"* / *"**SendInput inserts messages here**"*, with `SendInput`'s arrow at the **bottom of "Inbound input messages"**; text: *"The trackpad software is supposed to be using SendInput so that the wheel message orders correctly with the other messages in your input queue… **messages posted with the PostMessage function are processed ahead of input**."*
   https://devblogs.microsoft.com/oldnewthing/20160715-00/?p=93885
4. Keyboard-driver magic-sequence post (2024-02-19):
   > The sequence must be pressed on a physical keyboard because it is the keyboard driver that recognizes the key sequence and triggers the crash screen. **Injecting the keys into the window manager is inserting the keypresses at far too high a level in the input stack.**
   https://devblogs.microsoft.com/oldnewthing/20240219-00/?p=109424

**Net model (all four quotes together):** `SendInput` enters the **same hardware input queue the device drivers post into** (hence "serially, not interspersed with real input"), drained by the **Raw Input Thread**, which runs low-level hooks *before* updating input state. But it sits **above the keyboard/mouse device class drivers** — so typematic auto-repeat, the scancode mapper, and driver-recognised magic sequences never see injected input. **This single model explains: (a) why `SendInput` orders correctly with real input; (b) why `GetAsyncKeyState` may not reflect the key immediately; (c) why injected keys do not auto-repeat (§B4); (d) why `PostMessage` input is ordered *ahead* of real input and never reaches the input state (§B7).**

**Keyboard Input Overview [VERIFIED, verbatim]:** *"The SendInput function works by **injecting a series of simulated input events into a device's input stream**. The effect is similar to calling the keybd_event or mouse_event function repeatedly, **except that the system ensures that no other input events intermingle with the simulated events**."* / *"To block keyboard and mouse input events from reaching applications, use BlockInput. **Note, the BlockInput function will not interfere with the asynchronous keyboard input-state table. This means that calling the SendInput function while input is blocked will change the asynchronous keyboard input-state table.**"* https://learn.microsoft.com/en-us/windows/win32/inputdev/about-keyboard-input

**`keybd_event` [VERIFIED, verbatim]:** *"Synthesizes a keystroke… **The keyboard driver's interrupt handler calls the keybd_event function.**"* / *"**Note: This function has been superseded. Use SendInput instead.**"* https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-keybd_event

**`SPI_SETBLOCKSENDINPUTRESETS` (0x1027) exists** — controls whether `SendInput` resets the screen-saver/display idle timer. Relevant if a macro tool must not keep the user's screen awake while idling. https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-systemparametersinfoa

**The deprecation note is real and verbatim** — it lives on the legacy pages (*"This function has been superseded. Use SendInput instead."*), not on the `SendInput` page.

**`mouse_event` is documented as superseded [VERIFIED, verbatim]:**
> The **mouse_event** function synthesizes mouse motion and button clicks. **Note: This function has been superseded. Use SendInput instead.**
https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-mouse_event

**And `mouse_event`'s Remarks give the *complete* documented pointer-acceleration pipeline — the missing second stage that `MOUSEINPUT` omits [VERIFIED, verbatim]:**
> Relative mouse motion is subject to the settings for mouse speed and acceleration level. An end user sets these values using the Mouse application in Control Panel. An application obtains and sets these values with the `SystemParametersInfo` function.
> The system applies two tests to the specified relative mouse motion when applying acceleration. If the specified distance along either the x or y axis is greater than the first mouse threshold value, and the mouse acceleration level is not zero, the operating system doubles the distance. If the specified distance along either the x- or y-axis is greater than the second mouse threshold value, and the mouse acceleration level is equal to two, the operating system doubles the distance that resulted from applying the first threshold test. It is thus possible for the operating system to multiply relatively-specified mouse motion along the x- or y-axis by up to four times.
> **Once acceleration has been applied, the system scales the resultant value by the desired mouse speed. Mouse speed can range from 1 (slowest) to 20 (fastest) and represents how much the pointer moves based on the distance the mouse moves. The default value is 10, which results in no additional modification to the mouse motion.**
https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-mouse_event

**⇒ The complete documented transform applied to a relative `SendInput` move is:**
```
output = speed_scale( threshold_double( threshold_double( input_delta ) ) )
where threshold_double ∈ {1x, 2x, 4x}  (from iMouseThreshold1/2 + iMouseSpeed)
and   speed_scale     = f(iMouseSensitivity), f(10) == 1.0
```
**To make synthetic relative motion deterministic you must neutralise both stages: query `SystemParametersInfo(SPI_GETMOUSE, ...)` (3 ints: threshold1, threshold2, speed) and invert them in your own delta computation, or set `MouseSpeed`(acceleration level)=0 so the doubling tests are skipped and `MouseSensitivity`=10 so the speed scale is 1.0.** If you don't, a 400-pixel synthetic flick can land as 400, 800 or 1600 pixels of cursor travel depending on the user's Control Panel settings.

---

### B2. Absolute vs relative mouse movement, and pointer ballistics

**Flags [VERIFIED, verbatim from `MOUSEINPUT`]:**
> `MOUSEEVENTF_MOVE` 0x0001 | Movement occurred.
> `MOUSEEVENTF_MOVE_NOCOALESCE` 0x2000 | The WM_MOUSEMOVE messages will not be coalesced. The default behavior is to coalesce WM_MOUSEMOVE messages.
> `MOUSEEVENTF_VIRTUALDESK` 0x4000 | Maps coordinates to the entire desktop. **Must be used with MOUSEEVENTF_ABSOLUTE.**
> `MOUSEEVENTF_ABSOLUTE` 0x8000 | The dx and dy members contain normalized absolute coordinates. If the flag is not set, dx and dy contain relative data (the change in position since the last reported position).

**Absolute mapping [VERIFIED, verbatim]:**
> If MOUSEEVENTF_ABSOLUTE value is specified, dx and dy contain **normalized absolute coordinates between 0 and 65,535**. The event procedure maps these coordinates onto the display surface. Coordinate (0,0) maps onto the upper-left corner of the display surface; coordinate (65535,65535) maps onto the lower-right corner. **In a multimonitor system, the coordinates map to the primary monitor.**
> If MOUSEEVENTF_VIRTUALDESK is specified, the coordinates map to the entire virtual desktop.

So the exact conversion is:
```
dx = round(x_screen * 65535.0 / (virtual_width  - 1));   // with MOUSEEVENTF_VIRTUALDESK, x relative to SM_XVIRTUALSCREEN
dy = round(y_screen * 65535.0 / (virtual_height - 1));
flags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
```

**Pointer ballistics — the definitive documented answer [VERIFIED, verbatim from the same page]:**
> **Relative mouse motion is subject to the effects of the mouse speed and the two-mouse threshold values.** A user sets these three values with the **Pointer Speed** slider of the Control Panel's **Mouse Properties** sheet. You can obtain and set these values using the `SystemParametersInfo` function.
>
> The system applies two tests to the specified relative mouse movement. If the specified distance along either the x or y axis is greater than the **first mouse threshold value**, and the **mouse speed is not zero**, the system **doubles the distance**. If the specified distance along either the x or y axis is greater than the **second mouse threshold value**, and the **mouse speed is equal to two**, the system **doubles the distance that resulted from applying the first threshold test**. It is thus possible for the system to multiply specified relative mouse movement along the x or y axis **by up to four times**.

**This single paragraph answers the whole question:**
- **`SendInput` with `MOUSEEVENTF_MOVE` *without* `ABSOLUTE` IS subject to ballistics.** The doc says so explicitly, in the `MOUSEINPUT` page — i.e. the `SendInput` path, not just `mouse_event`.
- **`SendInput` with `MOUSEEVENTF_ABSOLUTE` is NOT subject to ballistics** — absolute coordinates are mapped directly onto the display surface; no threshold/doubling language applies. **(The absence of an explicit "absolute bypasses ballistics" sentence is a documentation gap; the positive statement about relative motion plus the direct-mapping language for absolute is the verified basis. Mark "absolute bypasses ballistics" as strongly implied by the docs, not literally stated.)**
- The scaling is **multiplicative and discontinuous**: ×1, ×2, or ×4 depending on thresholds and speed. It is *not* a smooth curve in the modern sense; the smooth `SmoothMouseXCurve`/`SmoothMouseYCurve` registry curves apply to the OS pointer (and to the "Enhance pointer precision" feature) while the `MOUSEINPUT` doubling tests are the legacy `MouseThreshold1/2` + `MouseSpeed` mechanism.

**Registry keys and defaults [VERIFIED values, from a Microsoft Q&A answer + Windows defaults]:**
`HKEY_CURRENT_USER\Control Panel\Mouse`:
```
"MouseSensitivity"="10"        ; "Pointer Speed" slider, 1..20 (10 = 6/11 notch)
"MouseSpeed"="1"               ; 1 = "Enhance pointer precision" ON, 0 = OFF
"MouseThreshold1"="6"
"MouseThreshold2"="10"
"SmoothMouseXCurve"=hex: ...   ; 5 points, 8 bytes each (2x 32-bit fixed point)
"SmoothMouseYCurve"=hex: ...
```
The **1:1 (no acceleration) default curve** as published in that answer:
```
"SmoothMouseXCurve"=hex:00,00,00,00,00,00,00,00, 00,a0,00,00,00,00,00,00, 00,40,01,00,00,00,00,00,
                        00,80,02,00,00,00,00,00, 00,05,00,00,00,00,00,00
"SmoothMouseYCurve"=hex:00,00,00,00,00,00,00,00, 66,a6,02,00,00,00,00,00, cd,4c,05,00,00,00,00,00,
                        a0,99,0a,00,00,00,00,00, 38,33,15,00,00,00,00,00
```
Read as little-endian 32-bit pairs: X = (0, 0x0000A000, 0x00014000, 0x00028000, 0x00050000) → (0, 40960, 81920, 163840, 327680); Y = (0, 0x0002A666, 0x00054CCD, 0x000A99A0, 0x00153338) → (0, 173670, 347341, 694688, 1389368). The Y/X ratio is 4.238 at every point ⇒ the stored curve is "4.238× gain on a 1:1 line", i.e. **the `MouseSensitivity` slider supplies the compensating factor and these two curves together define the identity response**.
**Caveat: this comes from a Microsoft Q&A *answer* (community, "Limitless Technology"), not from a Microsoft specification page. Treat the exact hex as a widely-reproduced Windows default rather than a normative spec — mark as VERIFIED-BY-REPRODUCTION, not VERIFIED-BY-SPECIFICATION.**
https://learn.microsoft.com/en-gb/answers/questions/1162444/how-to-further-customize-mouse-acceleration-curve

**The normative document exists but is archived: Microsoft's *"Pointer Ballistics for Windows XP"* whitepaper, MSDN `gg463319`** — still cited by the current DirectX article (*"For more information about applying pointer ballistics, see Pointer ballistics for Windows XP"*). Archived copies (unreachable from this session; content is translation-derived and therefore **UNVERIFIED verbatim**):
https://web.archive.org/web/20110421045930/http://msdn.microsoft.com/en-us/windows/hardware/gg463319.aspx
https://web.archive.org/web/20041104014442/http://www.microsoft.com/whdc/device/input/pointer-bal.mspx
Its content, per a translation: the transfer function is **five points stored as five 16.16 fixed-point XY pairs**; the algorithm is a **six-point lookup table (first point `[0,0]`) with linear interpolation**; the acceleration multiplier is computed from the **magnitude of the (x,y) vector** (not per-axis, so diagonals are not biased); **16.16 fixed-point integer math**; the **division remainder is carried into the next mouse packet** ("super-pixel positioning"); and speeds beyond ~4 in/s are **extrapolated linearly**. https://www.0xaa55.com/forum.php?mod=viewthread&action=printable&tid=25841

**The *default* (acceleration-ON) curve — identical bytes from two independent community sources** (Windows 8.1 / 10 / 11), 40-byte `REG_BINARY` = five little-endian QWORDs whose low 32 bits are 16.16 fixed point:
```
SmoothMouseXCurve = 00,00,00,00,00,00,00,00  15,6e,00,00,00,00,00,00  00,40,01,00,00,00,00,00  29,dc,03,00,00,00,00,00  00,00,28,00,00,00,00,00
SmoothMouseYCurve = 00,00,00,00,00,00,00,00  fd,11,01,00,00,00,00,00  00,24,04,00,00,00,00,00  00,fc,12,00,00,00,00,00  00,c0,bb,01,00,00,00,00
```
Decoded: X ≈ 0.0, 0.43, 1.25, 3.86, 40.0; Y ≈ 0.0, 1.0702, 4.1406, 18.9844, 443.75 → **gain per segment ≈ 2.5× → 3.3× → 4.9× → 11×**, i.e. a strongly progressive curve. That is what "Enhance pointer precision" actually is, and it is why synthetic relative motion is non-linear by default.
https://sugarsweetapps.com/blog/how-to-customize-mouse-acceleration-in-windows-11-smoothmousexcurve-and-smoothmouseycurve/ , https://esreality.com/post/2971068/
Registry path for all of these: `HKEY_CURRENT_USER\Control Panel\Mouse`. **No Microsoft Learn page documents `SmoothMouseXCurve`/`SmoothMouseYCurve` → UNVERIFIED as officially documented.**

`SystemParametersInfo` `SPI_GETMOUSE` returns the 3 integers (`iMouseThreshold1`, `iMouseThreshold2`, `iMouseSpeed`) into an `int[3]`; `SPI_SETMOUSE` writes them. **The `SPI_GETMOUSE`/`SPI_SETMOUSE` flag names and the int[3] layout are stated in the MOUSEINPUT remark ("obtain and set these values using the SystemParametersInfo function"); I did not retrieve the `SystemParametersInfo` page itself in this pass — mark the exact parameter-block layout as UNVERIFIED here.**

**Raw input and ballistics — NOW VERIFIED (this was previously marked UNVERIFIED; here is the primary quote):**

`RAWMOUSE` docs [VERIFIED, verbatim, sic — the grammar error is Microsoft's]:
> **In contrast to legacy WM_MOUSEMOVE window messages Raw Input mouse events is not subject to the effects of the mouse speed set in the Control Panel's Mouse Properties sheet.**
https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-rawmouse

Microsoft's DirectX technical article *"Taking Advantage of High-Definition Mouse Movement"* [VERIFIED, verbatim]:
> The primary disadvantage to data from `WM_MOUSEMOVE` is that it is limited to the screen resolution. This means that if you move the mouse slightly — but not enough to cause the pointer to move to the next pixel — then no `WM_MOUSEMOVE` message is generated.
> **The advantage to WM_MOUSEMOVE, however, is that Windows applies pointer acceleration (also known as ballistics) to the raw mouse data**, which makes the mouse pointer behave as customers expect.
> **The advantage to using WM_INPUT is that your game receives raw data from the mouse at the lowest level possible. The disadvantage is that WM_INPUT has no ballistics applied to its data**, so if you want to drive a cursor with this data, extra effort will be required to make the cursor behave like it does in Windows.
> **Internally, DirectInput creates a second thread to read WM_INPUT data** … DirectInput is only useful for reading data from DirectInput joysticks… **Overall, using DirectInput offers no advantages when reading data from mouse or keyboard devices, and the use of DirectInput in these scenarios is discouraged.**
https://learn.microsoft.com/en-us/windows/win32/dxtecharts/taking-advantage-of-high-dpi-mouse-movement

**⇒ So the full, verified picture is a clean split:**
| Path | Ballistics applied? |
|---|---|
| `WM_MOUSEMOVE` (desktop cursor path) | **YES** — *"Windows applies pointer acceleration (also known as ballistics) to the raw mouse data"* |
| `WM_INPUT` / raw input / DirectInput | **NO** — *"Raw Input mouse events is not subject to the effects of the mouse speed"* |
| `SendInput` relative (`MOUSEEVENTF_MOVE`, no ABSOLUTE) | **YES** — documented on `MOUSEINPUT` |
| `SendInput` absolute (+`ABSOLUTE`, ±`VIRTUALDESK`) | Not scoped to acceleration by any doc → bypasses (strongly implied, never literally stated) |

**Driver-side counterpart [VERIFIED, WDK]:** for absolute pointing devices the driver must scale `LastX = ((device input x value) * 0xFFFF) / (Maximum x capability of the device)` (same for Y), set `MOUSE_MOVE_ABSOLUTE`, and: *"If the input should be mapped by Window Manager to an entire virtual desktop, the driver sets the MOUSE_VIRTUAL_DESKTOP flag in Flags. **If the MOUSE_VIRTUAL_DESKTOP flag isn't set, Window Manager maps the input to only the primary monitor.**"* https://learn.microsoft.com/en-us/windows-hardware/drivers/hid/keyboard-and-mouse-class-drivers

**What games actually read:** `RAWMOUSE` flags are `MOUSE_MOVE_RELATIVE 0x00 / MOUSE_MOVE_ABSOLUTE 0x01 / MOUSE_VIRTUAL_DESKTOP 0x02 / MOUSE_ATTRIBUTES_CHANGED 0x04 / MOUSE_MOVE_NOCOALESCE 0x08`. Microsoft explicitly recommends `WM_INPUT` over DirectInput for mouse/keyboard.

**The practical, verified conclusion for the RPA tool: if the game uses raw input, relative `SendInput` moves are the only path that produces motion the game can see. The desktop ballistics settings apply to the *cursor* path, and the *raw* path bypasses them — but `SendInput` relative moves are themselves documented as ballistics-scaled, so if the target also (or instead) reads `WM_MOUSEMOVE`, or if you drive the cursor yourself, you must neutralise ballistics: (a) set acceleration level 0 and speed 10, (b) read `SPI_GETMOUSE`/`SPI_GETMOUSESPEED` and invert the documented transform in your own delta computation, or (c) use a driver-level injector (§B7).**

---

### B3. `SetCursorPos` vs `SendInput`

**`SetCursorPos` documented remarks [VERIFIED, verbatim]:**
> The cursor is a shared resource. A window should move the cursor only when the cursor is in the window's client area.
> The calling process must have **`WINSTA_WRITEATTRIBUTES`** access to the window station.
> The input desktop must be the current desktop when you call **SetCursorPos**. Call `OpenInputDesktop` to determine whether the current desktop is the input desktop. If it is not, call `SetThreadDesktop` with the HDESK returned by OpenInputDesktop to switch to that desktop.
https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setcursorpos

**⚠️ IMPORTANT NEGATIVE FINDING — the brief's premise is wrong:** the current `SetCursorPos` page contains **no remarks about DirectInput, raw input, or the input queue.** Confirmed against the doc's source of truth (`-remarks` block is exactly the three paragraphs above): https://raw.githubusercontent.com/MicrosoftDocs/sdk-api/docs/sdk-api-src/content/winuser/nf-winuser-setcursorpos.md **The "SetCursorPos doesn't work with DirectInput" property is real but community-documented only — it is NOT in the API docs.**

**What this means concretely:**
- `SetCursorPos` warps the *cursor* and generates `WM_MOUSEMOVE`-level desktop messages. It does **not** synthesize a HID-level relative motion event.
- **A game reading Raw Input / DirectInput mouse state will see no movement delta at all from `SetCursorPos`.** This is the classic "SetCursorPos does nothing in this game" failure. **UNVERIFIED as an explicit statement in a Microsoft doc** — but it follows directly from the verified fact that raw input is device-level (B2) while `SetCursorPos` operates on the desktop cursor (B3), and it is consistent with the `SetCursorPos` remarks warning about `WINSTA_WRITEATTRIBUTES` / input-desktop requirements rather than any device-level injection.
- **`SetCursorPos` also does not go through UIPI** the way `SendInput` does (it is a window-station operation, not input injection) — **[UNVERIFIED, inferred; not stated in a retrieved doc]**.
- `SetCursorPos` is subject to `ClipCursor` — *"If the new coordinates are not within the screen rectangle set by the most recent ClipCursor function call, the system automatically adjusts the coordinates."* **This matters: many games call `ClipCursor` to the window rect or to the monitor while capturing the mouse.** Same URL.
- `ClipCursor` also has a documented UIPI note: *"Starting with Windows Vista, `ClipCursor` is subject to UIPI and will fail if the calling process is at a lower integrity level than the process that set the clip rectangle"* — relevant if you try to undo a game's clip. https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-clipcursor

**Recommendation: for game automation, `SendInput` with relative `MOUSEEVENTF_MOVE` is the primary path; `SetCursorPos` is only a fallback for non-raw-input (Win32 message-based) targets, and even then you should follow it with a synthetic `WM_MOUSEMOVE` if the app needs one.**

---

### B4. Keyboard: scan codes, extended keys, auto-repeat, holds

**Flags [VERIFIED, verbatim from `KEYBDINPUT`]:**
> `KEYEVENTF_EXTENDEDKEY` 0x0001 | If specified, the wScan scan code consists of a sequence of two bytes, where the first byte has a value of 0xE0.
> `KEYEVENTF_KEYUP` 0x0002 | If specified, the key is being released. If not specified, the key is being pressed.
> `KEYEVENTF_SCANCODE` 0x0008 | If specified, **wScan identifies the key and wVk is ignored.**
> `KEYEVENTF_UNICODE` 0x0004 | If specified, the system synthesizes a `VK_PACKET` keystroke. The wVk parameter must be zero. **This flag can only be combined with the KEYEVENTF_KEYUP flag.**
> Set the `KEYEVENTF_SCANCODE` flag to define keyboard input in terms of the scan code. **This is useful for simulating a physical keystroke regardless of which keyboard is currently being used.** You can also pass the `KEYEVENTF_EXTENDEDKEY` flag if the scan code is an extended key. **The virtual key value of a key can change depending on the current keyboard layout or what other keys were pressed, but the scan code will always be the same.**
https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-keybdinput

**Why scan codes matter [VERIFIED reasoning from the doc above]:** scan codes are layout-independent and physically meaningful. Games that use DirectInput get **scan codes** (DirectInput keyboard device objects are created with `GUID_SysKeyboard` and report `DIK_*` scan-code-based constants), so a `VK`-only `SendInput` can produce the wrong physical key on non-US layouts and, more importantly, is a *different* path than a scan-code event. **"DirectInput reads scan codes" is architectural knowledge I did not retrieve a primary Microsoft quote for — mark the mechanism as UNVERIFIED-in-this-pass, while the recommendation (always populate `wScan` and set `KEYEVENTF_SCANCODE`) follows directly from the verified `KEYBDINPUT` text.**
- **Auto-repeat is a DEVICE/DRIVER feature, not an API feature — [VERIFIED, WDK], which is the proof that `SendInput` cannot auto-repeat:**
> `typedef struct _KEYBOARD_TYPEMATIC_PARAMETERS { USHORT UnitId; USHORT Rate; USHORT Delay; }`
> **Rate** — Specifies the rate at which character output from a keyboard repeats, **in characters per second, after a key is pressed and continuously held down.** … The default value is `KEYBOARD_TYPEMATIC_RATE_DEFAULT`.
> **Delay** — Specifies the amount of time that must elapse, **in milliseconds, after a key is pressed and continuously held down, before the character output from a keyboard begins to repeat.** … The default is `KEYBOARD_TYPEMATIC_DELAY_DEFAULT`.
Used with `IOCTL_KEYBOARD_QUERY_TYPEMATIC` / `IOCTL_KEYBOARD_SET_TYPEMATIC` — i.e. it lives in the **keyboard class driver**, which §B1's stack model places *below* `SendInput`'s insertion point. The numeric `KEYBOARD_TYPEMATIC_*_MINIMUM/MAXIMUM/DEFAULT` values are not published on that page (they live in `ntddkbd.h`) → UNVERIFIED here.
https://learn.microsoft.com/en-us/windows/win32/api/ntddkbd/ns-ntddkbd-keyboard_typematic_parameters , https://learn.microsoft.com/en-us/windows/win32/api/ntddkbd/ni-ntddkbd-ioctl_keyboard_set_typematic

Windows' own model [VERIFIED, verbatim]: *"Key-up and key-down messages typically occur in pairs, but **if the user holds down a key long enough to start the keyboard's automatic repeat feature, the system generates a number of WM_KEYDOWN or WM_SYSKEYDOWN messages in a row**."* / *"**The previous key-state flag … is set to 1 for WM_KEYDOWN and WM_SYSKEYDOWN keystroke messages generated by the automatic repeat feature.**"* (that flag is `KF_REPEAT 0x4000`). Note the wording: it is *the user* holding a *physical* key. https://learn.microsoft.com/en-us/windows/win32/inputdev/about-keyboard-input

**Scan-code table [VERIFIED, Microsoft; "The Scan 1 Make code is delivered in WM_KEYDOWN/WM_KEYUP/WM_SYSKEYDOWN/WM_SYSKEYUP and WM_INPUT messages"]:** A `0x001E`, W `0x0011`, Z `0x002C`, Space `0x0039`, Enter `0x001C`, Esc `0x0001`, LeftShift `0x002A`, **RightShift `0x0036`**, LeftControl `0x001D`, **RightControl `0xE01D`**, LeftAlt `0x0038`, **RightAlt `0xE038`**, LWin `0xE05B`, RWin `0xE05C`, Insert `0xE052`, Home `0xE047`, PgUp `0xE049`, Delete `0xE053`, End `0xE04F`, PgDn `0xE051`, Up `0xE048`, Down `0xE050`, Left `0xE04B`, Right `0xE04D`, keypad Enter `0xE01C`, keypad `/` `0xE035`, PrintScreen `0xE037`, Pause `0xE11D45`/`0xE046`/`0x0045`, NumLock `0x0045`. Extended keys = right-hand Alt/Ctrl, Insert/Delete/Home/End/PgUp/PgDn/arrows, Break, PrintScreen, keypad `/` and Enter, Windows and Application keys — **plus the documented trap: *"The right-hand Shift key is not considered an extended key either; it has a separate scan code (0x36) instead."*** And the definitive games rationale [VERIFIED, verbatim]: *"**scan codes might be required in specific cases when you need to know which key is pressed regardless of the current keyboard layout. For example, the WASD (W is up, A is left, S is down, and D is right) key bindings for games, which ensure a consistent key formation across US QWERTY or French AZERTY keyboard layouts.**"* Same URL.

**DirectInput keyboard = scan codes [VERIFIED, verbatim, Microsoft's own DirectInput docs]:**
> **In one important respect, DirectInput applications read the keyboard differently from the way Windows does. For DirectInput applications, keyboard data refers not to virtual keys but to the actual physical keys - that is, the scan codes. DIK_ENTER, for example, refers only to the ENTER key on the main keyboard, not to the ENTER key on the numerical keypad.**
https://learn.microsoft.com/en-us/previous-versions/windows/desktop/ee418271(v=vs.85)
**⇒ This is the authoritative basis for "always send `KEYEVENTF_SCANCODE` to a DirectInput game": its bindings are `DIK_*` scan codes, so a `wVk`-only injection carries no scancode at all and cannot satisfy a scan-code binding.**

**`MapVirtualKey` uMapType table [VERIFIED, verbatim]:**
| Flag | Value | Documented behaviour |
|---|---|---|
| `MAPVK_VK_TO_VSC` | 0 | *"…translated into a scan code. **If it is a virtual-key code that does not distinguish between left- and right-hand keys, the left-hand scan code is returned.** If there is no translation, the function returns 0."* |
| `MAPVK_VSC_TO_VK` | 1 | …a virtual-key code that **does not distinguish** between left/right. Vista+: the **high byte of `uCode` may contain `0xe0` or `0xe1`** to specify an extended scan code. |
| `MAPVK_VK_TO_CHAR` | 2 | unshifted character; *"Dead keys (diacritics) are indicated by setting the top bit of the return value."* |
| `MAPVK_VSC_TO_VK_EX` | 3 | *"…a virtual-key code that **distinguishes between left- and right-hand keys**."* |
| `MAPVK_VK_TO_VSC_EX` | 4 | Vista+. *"If the scan code is an extended scan code, the high byte of the returned value will contain either 0xe0 or 0xe1."* |
Remarks: pass `VK_LSHIFT`/`VK_RSHIFT`/`VK_LCONTROL`/`VK_RCONTROL`/`VK_LMENU`/`VK_RMENU` (not `VK_SHIFT`/`VK_CONTROL`/`VK_MENU`) to get left/right scan codes. Plain `MapVirtualKey` does **not** accept the `0xe0`/`0xe1` prefix in the VSC→VK direction — use the `_EX` variants.
https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-mapvirtualkeya , https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-mapvirtualkeyexw

**Auto-repeat [VERIFIED by API semantics — you must implement it yourself]:**
`SendInput` inserts *the events you give it*. `KEYBDINPUT` has no repeat field, no rate, no count — a single `KEYEVENTF_KEYUP`-less struct is **one** key-down event. **There is nothing in the API that generates a typematic repeat.** Therefore:
- **A key hold = one down event … wait … one up event.** The game's own input polling sees the key as continuously down between them (raw input / DirectInput report *state*, not just transitions), so a hold works **provided the game polls state rather than counting WM_KEYDOWN messages**.
- **Auto-repeat = you must send repeated down (and optionally up/down) events on your own timer.** If the target reads `WM_KEYDOWN` and expects Windows' typematic repeat, you must synthesize the cadence; Windows will not do it for injected keys.
- The OS's own typematic settings are `SPI_GETKEYBOARDDELAY` (0..3 → 250/500/750/1000 ms) and `SPI_GETKEYBOARDSPEED` (0..31 → ~2.5..30 repeats/s). **These do NOT apply to `SendInput`-injected keys** — again because there is no repeat generation in the API. **[The claim "typematic settings do not apply to injected keys" is inferred from the API having no repeat mechanism; mark the explicit statement UNVERIFIED.]**
- `KEYEVENTF_UNICODE` is documented as *"only … combined with the KEYEVENTF_KEYUP flag"* and sends `WM_KEYDOWN`/`WM_KEYUP` with `wParam == VK_PACKET` — i.e. it is a **message-level**, text-oriented path. **It is useless for games** (no scan code, no device-level event).

---

### B5. Frame-timing-aware pacing: why `Sleep()` fails

**Timer resolution [VERIFIED, verbatim from `timeBeginPeriod`]:**
> Prior to Windows 10, version 2004, this function affects a global Windows setting. For all processes Windows uses the lowest value (that is, highest resolution) requested by any process. **Starting with Windows 10, version 2004, this function no longer affects global timer resolution.** For processes which call this function, Windows uses the lowest value (that is, highest resolution) requested by any process. **For processes which have not called this function, Windows does not guarantee a higher resolution than the default system resolution.**
> **Starting with Windows 11, if a window-owning process becomes fully occluded, minimized, or otherwise invisible or inaudible to the end user, Windows does not guarantee a higher resolution than the default system resolution.**
> Setting a higher resolution can improve the accuracy of time-out intervals in wait functions. However, it can also reduce overall system performance, because **the thread scheduler switches tasks more often**. High resolutions can also prevent the CPU power management system from entering power-saving modes. Setting a higher resolution does not improve the accuracy of the high-resolution performance counter.
https://learn.microsoft.com/en-us/windows/win32/api/timeapi/nf-timeapi-timebeginperiod

**Three critical consequences:**
1. **Windows 10 2004+ made timer resolution per-process.** Your process must call `timeBeginPeriod` **itself** — you can no longer piggyback on another app's request. (This reversed the pre-2004 behaviour where any process could raise the global resolution.)
2. **Windows 11: if your automation window is occluded/minimized/invisible, you may silently lose the high resolution.** For a background RPA tool this is a real trap — a hidden window can degrade your pacing.
3. The default system timer tick is **15.625 ms (64 Hz)**, which is the origin of the "Sleep(1) sleeps ~15.6 ms" behaviour. **Measured and documented by Bruce Dawson (ex-Google/Valve/Microsoft), [MEASURED], verbatim:**
   > The interval between timer interrupts depends on the Windows version and on your hardware but **on every machine I have used recently the default interval has been 15.625 ms (1,000 ms divided by 64)**. That means that **if you call `Sleep(1)` at some random time then you will probably be woken sometime between 1.0 ms and 16.625 ms in the future**, whenever the next interrupt fires (or the one after that if the next interrupt is too soon).
   > The default timer resolution on Windows is **15.6 ms – a timer interrupt 64 times a second.**
   Sysinternals `clockres` sample output from the same author: `Maximum timer interval: 15.600 ms / Minimum timer interval: 0.500 ms / Current timer interval: 1.000 ms`.
   https://randomascii.wordpress.com/2020/10/04/windows-timer-resolution-the-great-rule-change/ , https://randomascii.wordpress.com/2013/07/08/windows-timer-resolution-megawatts-wasted/

**⚠️ Do not trust `clockres` / TimerTool for diagnostics [MEASURED, verbatim]:**
> With the latest version of Windows 10 I see that clockres claims that the timer interrupt frequency goes up to 1 kHz when I unplug my laptop. TimerTool.exe says the same thing. ETW tracing and looking at the Microsoft-Windows-Kernel-Power provider shows that **the OS itself is modifying the timer interrupt frequency (SystemTimeResolutionKernelChange events) about 30 times a second.** … they make clockres and TimerTool useless.
To measure properly, the same author ships `trace_timer_intervals.bat` (ETW, `Microsoft-Windows-Kernel-Power` provider, `SystemTimeResolutionChange` events). https://github.com/google/UIforETW/blob/main/bin/trace_timer_intervals.bat

**Cost of raising the timer frequency [MEASURED, same author]:**
- Throughput: *"The overhead that I measured varied from **2.5% to 5%**. That's about an order of magnitude more than I expected."* (measured with a busy-loop iteration counter; a clear ~5% step down when the frequency was raised, on both a laptop on battery and a workstation on wall power).
- Power: *"On my Windows 7 Sandybridge laptop it consistently shows a **.3 W** increase in power draw from having the timer frequency increased. That's almost **10% of the idle CPU package power draw**."*

**Windows 10 2004 "Great Rule Change" — the measured behaviour change [MEASURED]:**
> Previously the delay for `Sleep(1)` in any process was simply the same as the timer interrupt interval (with an exception for `timeBeginPeriod(1)`)… In Windows 10 2004 the mapping between `timeBeginPeriod` and the sleep delay in another process (one that didn't call `timeBeginPeriod`) is peculiar.
> [With an 8 ms interrupt interval] `Sleep(1)` … returns after one interval about 20% of the time and after two intervals the rest. Therefore three calls to `Sleep(1)` resulting in a average delay of **14.5 ms**.
> This behavior also seems to apply to `CreateWaitableTimerEx` and its … `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` flag.
**⇒ Requirement: your process must call `timeBeginPeriod` itself. If you rely on another process (a game, Chrome, a launcher) having raised the resolution, you will silently get ~15 ms sleeps on Windows 10 2004+, and on Windows 11 you additionally lose it when your window is occluded/minimized (per the `timeBeginPeriod` doc above).**

**High-resolution waitable timers [VERIFIED, verbatim from `CreateWaitableTimerExW`]:**
> `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` 0x00000002 | **Creates a high resolution timer. Use this value for time-critical situations when short expiration delays on the order of a few milliseconds are unacceptable. This value is supported in Windows 10, version 1803, and later.**
https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-createwaitabletimerexw

This is the correct primitive for sub-frame pacing: `CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS)` → `SetWaitableTimerEx` → `WaitForSingleObject`. It requires **no** `timeBeginPeriod` and is **not** affected by the per-process timer-resolution changes.
`SetWaitableTimerEx` companion facts [VERIFIED]: `lpDueTime` is *"in 100 nanosecond intervals … Negative values indicate relative time. **The actual timer accuracy depends on the capability of your hardware.**"*; `lPeriod` in ms; and **`TolerableDelay` — *"The tolerable delay for expiration time, in milliseconds"* — pass 0 when you need exactness**, because this is the coalescing slack Windows may add. https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-setwaitabletimerex

**MEASURED precision — now available from two independent sources:**

**1. Microsoft's own number [VERIFIED, verbatim]:**
> **The Go Windows port added support for high-resolution timers in Go 1.23, boosting resolution from ~15.6ms to ~0.5ms.**
> The problem is that this function's resolution is ~15.6ms, which isn't sufficient for the `time` package.
Microsoft DevBlogs, *"High-Resolution Timers on Windows"* (Quim Muntal, Microsoft Go team, 2024-10-01) — uses `CreateWaitableTimerExW(0, 0, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, …)` + `NtAssociateWaitCompletionPacket` + IOCP. https://devblogs.microsoft.com/go/high-resolution-timers-windows/

**2. Independent measured table [MEASURED — community, `haesy/rust_waitable_timer`, code later merged into Rust std via https://github.com/rust-lang/rust/pull/116461].** All values are µs, `expected → measured`:**
| Requested | plain `thread::sleep` (no `timeBeginPeriod`) | high-res waitable timer |
|---|---|---|
| 10000 | 18715, 15782, 15402, 15664, 15593 | 10571, 10074, 10875, 10074, 10900 |
| 3000 | 15462, 14625, 14608, 15911, 14839 | 3256, 3484, 3862, 3834, 3695 |
| 2000 | 14917, 15307, 15147, 14689, 15478 | 2636, 2645, 2871, 2673, 2645 |
| 1000 | 15405, 14316, 16011, 14736, 15337 | 1928, 1518, 1766, 1848, 1846 |
| **500** | **15502, 15138, 15170, 15265, 15181** | **528, 530, 532, 529, 530** |
| **250** | **14564, 15121, 14803, 15230, 14747** | **523, 521, 521, 521, 520** |
(With `timeBeginPeriod(1)` + plain `thread::sleep`: 500 → 1545, 1685, 1792, 1793; 250 → 1534, 1793, 1796, 1795. High-res timer after `timeBeginPeriod(1)`: 500 → 532, 524, 521, 540; 250 → 522, 522, 521, 521.)
https://github.com/haesy/rust_waitable_timer
**⇒ Reading: a high-resolution waitable timer gives a hard floor of roughly **0.52 ms** with ~0.5–2 ms overshoot; plain `Sleep` quantizes *every* request ≤ ~3 ms to the ~15.6 ms tick.** (Caveat: the repo's README does not show which code branch sets the flag; `winsleep.rs` was not fetchable → the flag attribution to the `windows_sleep` rows is UNVERIFIED, though the README states the repo is a test of `CreateWaitableTimerExW` and the code landed in Rust's high-resolution sleep PR.)

**Still NO primary-source number for `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` sub-millisecond *guarantees*** — the docs promise only *"short expiration delays on the order of a few milliseconds."* Measure your own pacer and log p50/p99 overshoot; don't trust published figures including dxcam's third-party "+/- 1ms".

**Pacing architecture for the RPA tool [engineering recommendation, derived from the verified facts]:**
- Do **not** do `capture(); detect(); click(); Sleep(fixed)` — the fixed sleep drifts and the phase relative to the game's frame boundary is arbitrary.
- Instead: block on the capture API's own signal. `AcquireNextFrame(timeout)` with a **small non-zero timeout** (e.g. 1–2 ms, or even `0` in a tight loop with a high-res timer sleep between attempts) means your wake-up *is* the new-frame event. This is exactly what OBS does (`AcquireNextFrame(0, …)` → `DXGI_ERROR_WAIT_TIMEOUT` ⇒ reuse previous frame) and what DXcam does.
- Then act immediately: `CopyResource` → `ReleaseFrame` (§A3) → detect → inject, with `age_ms` (§A9) computed at decision time. A high-resolution waitable timer is only needed to pace *idle* polling, not to time the action.
- **Never** sleep while holding the duplication frame — that is precisely the pattern Microsoft's `ReleaseFrame` remark warns against.

---

### B6. Exclusive fullscreen vs borderless windowed

**Verified facts:**
- DDA: *"Switch from DWM on, DWM off, or other full-screen application"* causes `DXGI_ERROR_ACCESS_LOST` and requires releasing and recreating `IDXGIOutputDuplication`. https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgioutputduplication-acquirenextframe
- GDI/`SetWindowDisplayAffinity` protection *"works only when the Desktop Window Manager (DWM) is composing the desktop"* — a fullscreen-exclusive app is by definition not in that path. https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowdisplayaffinity
- OBS' own source comment about display capture: *"completely shut down monitor capture if not in use, otherwise it can sometimes generate system lag **when a game is in fullscreen mode**"*, with a 3-second reinit interval — i.e. OBS treats fullscreen as a degraded/reinit-prone state. https://raw.githubusercontent.com/obsproject/obs-studio/master/plugins/win-capture/duplicator-monitor-capture.c
- OBS KB recommends Game Capture (a hook, not a screen capture) for games, *"should always be tried first"* — implicitly because screen capture of games is the weaker path. https://obsproject.com/kb/game-capture-source
- `trt-aimbot` troubleshooting, verbatim: **"Make sure the game is running in windowed mode, not fullscreen"** and *"Some games with anti-cheat may block screen capture tools."* https://github.com/shaarmander/trt-aimbot
- OBS "multi-adapter compatibility" string: *"If the preview is blank, make sure %name% is running on the same GPU as OBS."* https://raw.githubusercontent.com/obsproject/obs-studio/master/plugins/win-capture/data/locale/en-US.ini

**Practical conclusion (borderless windowed is preferred by bots):** borderless windowed keeps the game inside DWM composition, which (a) keeps DDA/WGC/BitBlt all functional, (b) removes the mode-transition `ACCESS_LOST` / reinit churn, (c) keeps the window `SetForegroundWindow`-able and message-routable, and (d) avoids the exclusive-mode input-capture semantics that can swallow or reroute synthetic input. **The specific claim "exclusive fullscreen games ignore SendInput" is UNVERIFIED — I found no primary source for it. The verified asymmetry is on the *capture* side, not the *input* side.** `SendInput` is documented as inserting into *"the keyboard or mouse input stream"* (a display-mode-agnostic description); the documented blockers are **UIPI/integrity level** and **foreground/focus**, not display mode.

**Microsoft's own position on the display modes [VERIFIED, verbatim]:**
DirectX Developer Blog, *"Demystifying Fullscreen Optimizations"* (Hannah Fisher, 2019-12-17):
> Games on PC generally offer three different types of display modes: **Fullscreen Exclusive (FSE), Windowed, and Borderless Windowed.** **Fullscreen Exclusive mode gives your game complete ownership of the display and allocation of resources of your graphics card.** In windowed game mode… **The Desktop Window Manager (DWM) has control of the display, while the graphics resources are shared among all applications**, unlike in a Fullscreen Exclusive environment.
> **With the release of Windows 10, we added Fullscreen Optimizations – which takes full screen exclusive games and runs them instead in a highly optimized borderless windowed format that takes up the entire screen.** You get the visual experience and performance of running your game in FSE, but **with the benefits of running in a windowed mode. These benefits include faster PC commands such as alt-tab, multiple monitor set ups and overlays.**
https://devblogs.microsoft.com/directx/demystifying-full-screen-optimizations/

*"For best performance, use DXGI flip model"* [VERIFIED, verbatim]:
> **Flip model presents go as far as making windowed mode effectively equivalent or better when compared to the classic 'fullscreen exclusive' mode.** In fact, you may want to reconsider whether your application actually needs a fullscreen exclusive mode, since **the benefits of a flip model borderless window include faster Alt-Tab switching and better integration with modern display features.**
> **Depending on window and buffer configuration, it is possible to bypass desktop composition entirely and directly send application frames to the screen, in the same way that exclusive fullscreen does.** … **DirectFlip**: Your swapchain buffers match the screen dimensions, and your window client region covers the screen. … **Once your swapchain has been 'DirectFlipped,' then the DWM can go to sleep**, and only wake up when something changes outside of your application. If other desktop contents come on top, the DWM can either seamlessly transition back to composed mode…
https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/for-best-performance--use-dxgi-flip-model

**⚠️ Critical caveat for the RPA tool, derived from the above:** because **Windows 10 Fullscreen Optimizations silently convert FSE into a full-screen borderless flip window**, and because **DirectFlip can put DWM to sleep / bypass composition entirely**, a game the user *believes* is in "exclusive fullscreen" may be in either state, and the capture path that works can differ between them. **There is no documented API that tells you which mode a third-party game is actually in.** Therefore: design the capture layer to *probe and fall back* (try DDA → on `DXGI_ERROR_UNSUPPORTED`/`ACCESS_LOST`/no output, try WGC → last resort BitBlt), re-probe on `ACCESS_LOST`, and request borderless-windowed from the user rather than assuming.

---

### B7. Anti-cheat and kernel-level input filtering

**How synthetic input is DETECTED [VERIFIED, verbatim from Microsoft]:**

`MSLLHOOKSTRUCT.flags`:
> The event-injected flags. An application can use the following values to test the flags. **Testing `LLMHF_INJECTED` (bit 0) will tell you whether the event was injected.** If it was, then testing `LLMHF_LOWER_IL_INJECTED` (bit 1) will tell you whether or not the event was injected from a process running at lower integrity level.
> `LLMHF_INJECTED` 0x00000001 | Test the event-injected (from any process) flag.
> `LLMHF_LOWER_IL_INJECTED` 0x00000002 | Test the event-injected (from a process running at lower integrity level) flag.
https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-msllhookstruct

`KBDLLHOOKSTRUCT.flags`:
> Testing **`LLKHF_INJECTED` (bit 4)** will tell you whether the event was injected. If it was, then testing **`LLKHF_LOWER_IL_INJECTED` (bit 1)** will tell you whether or not the event was injected from a process running at lower integrity level.
> bit 1: … **Note that bit 4 is also set whenever bit 1 is set.**
> bit 4: Specifies whether the event was injected. … **Note that bit 1 is not necessarily set when bit 4 is set.**
https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-kbdllhookstruct

**This is the single most important fact in Part B for an anti-cheat context: every `SendInput`-injected event is trivially and officially distinguishable from real hardware input via a documented flag, by any process with a low-level hook.** There is no supported way to clear these bits via `SendInput`.

**UIPI / integrity level:** `SendInput` *"is subject to UIPI. Applications are permitted to inject input only into applications that are at an equal or lesser integrity level"* and *"fails when it is blocked by UIPI"* with **no diagnostic in `GetLastError` or the return value beyond `0`**. https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-sendinput

**Interception driver (https://github.com/oblitum/Interception) [VERIFIED from README]:**
> Source code is built upon **Windows Driver Kit Version 7.1.0**. … **Tested from Windows XP to Windows 10.**
> Drivers can be installed through the command line installer, but **driver installation requires execution inside a prompt with administrative rights.** Run `install-interception` without any arguments inside an console executed as administrator…
> **License:** Interception is dual-licensed. For non-commercial purposes it adopts **LGPL** … For commercial purposes it adopts two other licenses … `Interception API License` (similar to non-commercial, removing restrictions for commercial usage; also includes an installer library so that driver installation can be embedded silently in your own installer) and `Interception License` (full source, including drivers and installers). Please contact me at francisco@oblita.com for acquiring a commercial license.
> **Use cases:** … **In game applications like BOTs and control customization.** …

Key points:
- It is a **kernel-mode filter driver** that sits below the user-mode input APIs, so injected events do not carry `LLMHF_INJECTED` in the low-level-hook sense. **[The precise claim "Interception-injected input does not set LLKHF_INJECTED" is architectural inference from "kernel filter driver below user-mode"; it is NOT stated in the README. Mark UNVERIFIED.]**
- **Commercial use requires a paid license** (LGPL only covers non-commercial). For a shipped product this is a real cost and legal item.
- **Driver signing:** a kernel driver must be WHQL/EV-signed to load on modern Windows with Secure Boot; the README documents WDK 7.1.0 and "tested from Windows XP to Windows 10" and says nothing about Windows 11 attestation signing. **UNVERIFIED whether current builds load on Windows 11 with Secure Boot enforced — this is a blocking question for the product.**
- The README's own list of use cases explicitly includes *"In game applications like BOTs"* — i.e. the author acknowledges the game-bot use case.

**`PostMessage`/`SendMessage` input is IGNORED — [VERIFIED, three Raymond Chen posts, verbatim]:**
1. *"You can't simulate keyboard input with PostMessage, revisited"* (2025-03-19):
   > **Posted messages masquerading as keyboard input don't come from the input queue; they come from the posted message queue. And the code that pulls messages from the posted message queue doesn't call the `WH_KEYBOARD` hook.**
   > the typical answers are either to use UI Automation to drive the target program, or if the target program's support for UI Automation is insufficient, you can use `SendInput` to generate synthetic input. **Synthetic input is treated like real input, and it goes through the input system like hardware input.**
   > Footnote: *"**Synthetic input can still be detected, for example, by looking for the `LLKHF_INJECTED` flag in the `KBDLLHOOKSTRUCT`'s flags.**"*
   https://devblogs.microsoft.com/oldnewthing/20250319-00/?p=110979
2. *"You can't simulate keyboard input with PostMessage"* (2005-05-30):
   > even if you manage to post the input messages into the target window's queue, that doesn't update the keyboard shift states. When the code behind the window calls the `GetKeyState` … or the `GetAsyncKeyState` … it's going to see the 'real' shift state and not the fake state that your posted messages have generated.
   > The `SendInput` function was designed for injecting input into Windows. If you use that function, then at least the shift states will be reported correctly.
   https://devblogs.microsoft.com/oldnewthing/20050530-11/?p=35513
3. *"Simulating input via WM_CHAR messages may fake out the recipient but it won't fake out the input system"* (2011-07-28): *"Note that the window manager still knows whether the input came from hardware or from `SendInput`."*
   https://devblogs.microsoft.com/oldnewthing/20110728-00/?p=10033
4. *"Why doesn't my keyboard hook get called for keyboard messages I manually posted?"* (2015-09-25): https://devblogs.microsoft.com/oldnewthing/20150925-00/?p=91511

**Raw Input vs `PostMessage` — the mechanism, [VERIFIED from Microsoft docs]:**
> By default, the operating system sends raw input from devices with the specified top level collection (TLC) to the registered application **as long as it has the window focus**.
> `RIDEV_INPUTSINK` 0x00000100: If set, this enables the caller to receive the input even when the caller is not in the foreground. Note that **hwndTarget** must be specified.
> `RIDEV_NOLEGACY` 0x00000030: If set, this prevents any devices specified by usUsagePage or usUsage from generating legacy messages… **if the mouse TLC is set with RIDEV_NOLEGACY, WM_LBUTTONDOWN and related legacy mouse messages are not generated.**
https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-rawinputdevice
> In the original input model, an application receives device-independent input in the form of messages that are sent or posted to its windows… In contrast, for raw input an application must register the devices it wants to get data from. … **An application gets the data directly from the device.**
https://learn.microsoft.com/en-us/windows/win32/inputdev/about-raw-input
**This is the verified mechanism by which `PostMessage`-based input can be inert for a game: with `RIDEV_NOLEGACY` the legacy mouse/keyboard messages are never generated at all, and the game reads the device directly.**

**`SetForegroundWindow` restrictions — [VERIFIED, verbatim], the exact condition list:**
> The system restricts which processes can set the foreground window. A process can set the foreground window by calling SetForegroundWindow only if: **All of the following conditions are true:** The calling process belongs to a desktop application, not a UWP app or a Windows Store app designed for Windows 8 or 8.1. / The foreground process has not disabled calls to SetForegroundWindow by a previous call to the LockSetForegroundWindow function. / No menus are active. — **Additionally, at least one of the following conditions is true:** The foreground lock time-out has expired (see SPI_GETFOREGROUNDLOCKTIMEOUT in SystemParametersInfo). / The calling process is the foreground process. / The calling process was started by the foreground process. / There is currently no foreground window, and thus no foreground process. / **The calling process received the last input event.** / Either the foreground process or the calling process is being debugged.
> **It is possible for a process to be denied the right to set the foreground window even if it meets these conditions.**
> An application cannot force a window to the foreground while the user is working with another window. Instead, Windows flashes the taskbar button of the window to notify the user.
https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setforegroundwindow

**`BlockInput` — [VERIFIED, verbatim]:**
> If this parameter is **TRUE**, keyboard and mouse input events are blocked… **Note that only the thread that blocked input can successfully unblock input.**
> When input is blocked, real physical input from the mouse or keyboard will not affect the input queue's synchronous key state (reported by GetKeyState and GetKeyboardState), nor will it affect the asynchronous key state (reported by GetAsyncKeyState). **However, the thread that is blocking input can affect both of these key states by calling SendInput. No other thread can do this.**
> The user presses **CTRL+ALT+DEL** or the system invokes the Hard System Error modal message box [unblocks it].
The page states **no privilege/integrity requirement** and does not mention UIPI or UIAccess → any claim that `BlockInput` requires admin/UIAccess is **UNVERIFIED**.
https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-blockinput

**The documented escape hatch for BOTH UIPI and foreground — `uiAccess="true"` [VERIFIED, verbatim]:**
> A process that's started with UIAccess rights has the following abilities: **Set the foreground window.** / **Drive any application window by using the SendInput function.** / **Use read input for all integrity levels by using low-level hooks, raw input, GetKeyState, GetAsyncKeyState, and GetKeyboardInput.** / Set journal hooks. / **Use AttachThreadInput to attach a thread to a higher integrity input queue.**
> **Windows enforces a PKI signature check on any interactive application that requests running with a UIAccess integrity level, regardless of the state of this security setting.**
> Secure locations: `\Program Files\` including subdirectories / `\Windows\system32\` / `\Program Files (x86)\`… The application must have a digital signature that can be verified by using a digital certificate that is associated with the Trusted Root Certification Authorities store on the local device.
https://learn.microsoft.com/en-us/previous-versions/windows/it-pro/windows-10/security/threat-protection/security-policy-settings/user-account-control-only-elevate-uiaccess-applications-that-are-installed-in-secure-locations
> **Only Accessibility or UI automation framework app sets the uiAccess flag to true to bypass the user interface privilege isolation (UIPI). To properly start app utilization, this flag must be Authenticode signed, and must reside in a protected location in the file system, namely Program Files.**
https://learn.microsoft.com/en-us/windows/win32/win_cert/certification-requirements-for-windows-desktop-apps
**Caveat: Microsoft explicitly scopes this to "Accessibility or UI automation framework" apps. Using it to automate games is outside the documented intent.**

**`ChangeWindowMessageFilterEx` does NOT help `SendInput` [VERIFIED]:** it only widens the **message** filter (`MSGFLT_ALLOW`), and it requires you to own the target window. Useless against a game's HWND.
https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-changewindowmessagefilterex

**Anti-cheat vendor statements — what is actually ON THE RECORD:**

*Riot Vanguard [VERIFIED OFFICIAL] — the strongest primary evidence that input injection is a tracked cheat class:*
> A "**pixelbot**" is a computer vision cheat that **injects player input** for the purposes of aiming at heads or casting spells with perfect timing. Coming in "**external**" (hardware microcontroller) and "**internal**" (python script) varieties, pixelbots can be extremely effective in VALORANT due to the low time-to-kill, sometimes just simply pulling the trigger for the cheater when an enemy enters their reticle (also known as a "triggerbot").
> **"On-demand" here means that Vanguard's driver component will no longer launch when the system starts** … [implying that before this change, `vgk.sys` DID launch at system start]
> Vanguard's Pre-Check requirements: Update to at least Windows 11 25H2… Enable UEFI Mode and Secure Boot… Enable Trusted Platform Module 2.0 (TPM)… Use Virtualization-Based Security (VBS) and Hypervisor-Protected Code Integrity (HVCI)… Enable Input-Output Memory Management Unit (IOMMU)
> A cheat driver intercepts any request a user-mode anti-cheat may make to the kernel, completely blinding it, and this is why most competitive games also install a driver anti-cheat component
https://www.riotgames.com/en/who-we-are/vanguard-on-demand (canonical; alternate path https://www.riotgames.com/en/news/vanguard-on-demand)
Vanguard Restrictions support page (link target confirmed inside Riot's own articles; content not retrievable): https://support-valorant.riotgames.com/hc/en-us/articles/22291331362067-Vanguard-Restrictions ; Vanguard Pre-Check: https://support.riotgames.com/en-us/riot/performance/vanguard-pre-check/
> Vanguard's strategy is simple: create a perimeter around the Windows kernel to ensure the system hasn't been compromised as early as we can. **If a cheat loads before we do, it has a better chance to hide where we can't find it.**
https://www.riotgames.com/en/news/vanguard-security-update-motherboard
Riot's own kernel-driver explainer article link: https://www.leagueoflegends.com/en-au/news/dev/dev-null-anti-cheat-kernel-driver/ **(content not retrievable — geo-restricted. UNVERIFIED.)**
**NOT FOUND: any official Riot statement that Vanguard specifically blocks/detects `SendInput` or macro tools by name.**

*BattlEye [VERIFIED OFFICIAL] — the clearest vendor policy on macro tools:*
> **I'm using the software XY while playing my game with BE enabled, is it allowed or can I get banned for it?** Generally we only ever ban for the use of actual cheats/hacks or components of such hacks which are designed to intentionally bypass BE's protection. Otherwise you don't need to worry about getting banned. For example, non-cheat overlays and visual enhancement tools like Reshade or SweetFX are generally supported unless desired otherwise by the game developers. **We might decide to kick (not ban) you at some point for using a specific program (such as macro tools), but that won't automatically flag you as a cheater.**
> **Failed to initialize BattlEye Service: Windows Test-Signing Mode not supported.** Please disable test-signing mode… **we cannot support systems running in test-signing mode in any way.**
> **…Kernel Debugging enabled.** Please disable Kernel Debugging… Afterwards you need to reboot your system.
> BattlEye is blocking certain software that is using **kernel drivers which contain known security issues** that can be exploited by cheats.
https://www.battleye.com/support/faq/
> Naturally the new system will employ a kernel-mode driver, but **BE never has been and never will be a rootkit** trying to hide its own activities on our users' computers.
https://www.battleye.com/2015/02/09/a-new-dawn/
**Actionable: enabling test-signing mode (a common requirement for loading a self-built filter driver) makes every BattlEye-protected game refuse to start at all.** Their public news index contains **no** DMA statement and **no** input-injection statement → any "BattlEye blocks DMA/mouse injection" claim is **UNVERIFIED**.

*EasyAntiCheat [NOT VERIFIED]:* only marketing pages are retrievable (https://www.easy.ac/ — *"prevention-first approach helps developers stop common exploits early, detect sophisticated threats"*); the developer docs at `dev.epicgames.com/docs/epic-online-services/trust-and-safety/anti-cheat-interfaces` return empty bodies (JS-rendered). **EAC's kernel-driver model, its integrity checks, and any EAC statement about input emulation are all UNVERIFIED here.**

**Verdict:** the *general* claim "anti-cheat blocks synthetic input" is **not** supported by any vendor statement found. What *is* verified: (a) every `SendInput` event is OS-flagged injected and any kernel driver can read that; (b) Riot officially classifies input-injecting pixelbots as cheats; (c) BattlEye officially reserves the right to *kick* (not ban) for macro tools, and *hard-refuses* test-signing/kernel-debugging; (d) synthetic input from a lower integrity level is silently dropped by UIPI. Assume detection is possible by construction; do not assume a specific vendor behaviour without a citation.

**Kernel driver signing — the framework that actually decides whether the Interception route ships [VERIFIED OFFICIAL]:**
> Starting with Windows 10, version 1607, **Windows will not load any new kernel-mode drivers which are not signed by the Dev Portal.**
> Cross-signed drivers are still permitted if any of the following are true: The PC was upgraded from an earlier release of Windows to Windows 10, version 1607. / **Secure Boot is off in the BIOS.** / **Driver was signed with an end-entity certificate issued prior to July 29th 2015** that chains to a supported cross-signed CA.
https://learn.microsoft.com/en-us/windows-hardware/drivers/install/kernel-mode-code-signing-policy--windows-vista-and-later-
**UNVERIFIED: whether the shipped `interception.sys` benefits from the pre-2015 grandfather clause, and whether `install-interception.exe /install` requires a reboot. The README says nothing about reboot, test-signing, or Secure Boot.**

**⚠️ Direct conflict with the project's own plan:** the workspace `AGENTS.md` states `interception.dll` / `.sys` ship via a separate `QuickScriptTool-HidDriver.zip` download. **BattlEye's FAQ makes test-signing mode incompatible with its games, and Microsoft's signing policy makes an unsigned/cross-signed driver non-loadable on Secure Boot machines.** This is a product decision that needs to be made explicitly, not discovered at runtime.

**Other verified Interception facts [from source, since the README does NOT contain the relative/absolute or raw-input claims the brief attributed to it]:**
```c
enum InterceptionMouseFlag
{
    INTERCEPTION_MOUSE_MOVE_RELATIVE      = 0x000,
    INTERCEPTION_MOUSE_MOVE_ABSOLUTE      = 0x001,
    INTERCEPTION_MOUSE_VIRTUAL_DESKTOP    = 0x002,
    INTERCEPTION_MOUSE_ATTRIBUTES_CHANGED = 0x004,
    INTERCEPTION_MOUSE_MOVE_NOCOALESCE    = 0x008,
    INTERCEPTION_MOUSE_TERMSRV_SRC_SHADOW = 0x100
};
#define INTERCEPTION_MAX_KEYBOARD 10
#define INTERCEPTION_MAX_MOUSE 10
```
(devices 1–10 keyboard, 11–20 mouse; API = `interception_create_context` / `_set_filter` / `_wait` / `_receive` / `_send`.)
https://raw.githubusercontent.com/oblitum/Interception/master/library/interception.h
- The installer binary is **`install-interception.exe`**, not "interception_install". Admin prompt required; **not** double-clickable.
- **The public source tree contains NO `driver/` and NO `installer/` directory** — consistent with the commercial license text that driver+installer *source* is a paid asset. The wiki is empty.
- **Documented device-ID exhaustion bug [COMMUNITY, downstream project]:** *"If you unplug / replug a device, or go into hibernate and resume, the Interception ID of a device will increase by 1. If the ID of a device goes above 10 (For keyboards) or 20 (For Mice), The device will completely cease to function until the next reboot… it is a limitation of the Interception driver."* https://github.com/oblitum/Interception/issues/25 , https://raw.githubusercontent.com/evilC/AutoHotInterception/master/README.md , https://raw.githubusercontent.com/jtroo/kanata/main/docs/platform-known-issues.adoc
- AutoHotInterception README on absolute mode: *"Note that Absolute mode will probably not work with FPS style mouse-aim games."* and *"Because Interception is a driver, and sits below windows proper, blocking with Interception goes so deep that it can even block CTRL+ALT+DEL."*

**Practical debug note [COMMUNITY, StackOverflow via API — verbatim]:**
> `SendInput` works for almost all games I have worked so far. But **for `SendInput` to work the game must be a foreground window.** … To solve that I used `PostMessage(hwnd,...)` … **But this does not work if game is using DirectInput.** That was solved by hooking `GetDeviceState`. Now another game I started working on is **using `WM_INPUT` or raw input and I have to create raw input to make it work.**
https://stackoverflow.com/questions/35408495/sendinput-to-background-window
**DirectInput keyboard gotcha [COMMUNITY, accepted answer, 22 votes, verbatim]:**
> **The keyup command was not working properly because when only sending the scan code, the keyup must be OR'ed with the scan code flag** (effectively enabling both flags) to tell the `SendInput()` API that this is a this is a both a KEYUP and a SCANCODE command.
https://stackoverflow.com/questions/3644881/simulating-keyboard-with-sendinput-api-in-directinput-applications
**⇒ For DirectInput games, send `KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP` (both flags), never `KEYEVENTF_KEYUP` alone with a `wVk`.**

---

## APPENDIX — URL INDEX

**Capture**
- https://learn.microsoft.com/en-us/windows/win32/gdi/capturing-an-image
- https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-bitblt
- https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowdisplayaffinity
- https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/desktop-dup-api
- https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgioutputduplication-acquirenextframe
- https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgioutputduplication-releaseframe
- https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/ns-dxgi1_2-dxgi_outdupl_frame_info
- https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgioutput-getframestatistics
- https://learn.microsoft.com/en-us/windows/win32/api/dxgi/ns-dxgi-dxgi_frame_statistics
- https://learn.microsoft.com/en-us/troubleshoot/windows-client/shell-experience/error-when-dda-capable-app-is-against-gpu
- https://learn.microsoft.com/en-us/windows/apps/develop/media-authoring-processing/screen-capture
- https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.graphicscapturesession
- https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.graphicscapturesession.minupdateinterval
- https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.graphicscapturesession.iscursorcaptureenabled
- https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.graphicscapturesession.isborderrequired
- https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.graphicscaptureaccess.requestaccessasync
- https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.direct3d11captureframepool.createfreethreaded
- https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.direct3d11captureframe.systemrelativetime
- https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/nf-dwmapi-dwmgetcompositiontiminginfo
- https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/ns-dwmapi-dwm_timing_info
- https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-map
- https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-copysubresourceregion
- https://learn.microsoft.com/en-us/windows/win32/direct3d11/d3d11-usages
- https://github.com/ra1nty/DXcam
- https://raw.githubusercontent.com/ra1nty/DXcam/46dfcbfe7aed2f00f38176c196c2a9e34249b545/README.md
- https://raw.githubusercontent.com/ra1nty/DXcam/c56045dd277aa44fe8e6a54f82ebbe7732d3cd86/README.md
- https://github.com/ra1nty/DXcam/pull/62 , /issues/8 , /issues/11 , /issues/13 , /issues/38 , /issues/67 , /issues/91 , /issues/109
- https://github.com/BoboTiG/python-mss
- https://raw.githubusercontent.com/BoboTiG/python-mss/main/src/mss/windows/gdi.py
- https://python-mss.readthedocs.io/stable/release-history/v10.2.0.html
- https://raw.githubusercontent.com/python-pillow/Pillow/main/src/display.c
- https://kylefu.me/2023/02/18/python-fast-screen-capture.html
- https://developer.nvidia.com/capture-sdk
- https://developer.nvidia.com/capture-sdk-software-license-agreement
- https://forums.developer.nvidia.com/t/nvfbc-on-geforce/54460
- https://raw.githubusercontent.com/keylase/nvidia-patch/master/README.md
- https://obsproject.com/forum/resources/obs-nvfbc.796/
- https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/AMF/master/amf/public/include/components/DisplayCapture.h
- https://gpuopen.com/archived/adl/
- https://raw.githubusercontent.com/obsproject/obs-studio/master/plugins/win-capture/duplicator-monitor-capture.c
- https://raw.githubusercontent.com/obsproject/obs-studio/master/libobs-d3d11/d3d11-duplicator.cpp
- https://raw.githubusercontent.com/obsproject/obs-studio/master/libobs-winrt/winrt-capture.cpp
- https://obsproject.com/kb/display-capture-sources
- https://github.com/obsproject/obs-studio/issues/13719
- https://arxiv.org/abs/2506.16042

**Input**
- https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-sendinput
- https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-mouseinput
- https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-keybdinput
- https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setcursorpos
- https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-clipcursor
- https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-msllhookstruct
- https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-kbdllhookstruct
- https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-mapvirtualkeyexw
- https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getrawinputdata
- https://learn.microsoft.com/en-gb/answers/questions/1162444/how-to-further-customize-mouse-acceleration-curve
- https://devblogs.microsoft.com/oldnewthing/20121206-00/?p=5903
- https://learn.microsoft.com/en-us/windows/win32/api/timeapi/nf-timeapi-timebeginperiod
- https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-createwaitabletimerexw
- https://raw.githubusercontent.com/oblitum/Interception/master/README.md
- https://github.com/shaarmander/trt-aimbot

**Blocked (Cloudflare 403 — cite URL only, no quote):** stackoverflow.com/questions/40808759, /44774169, /44403173
