import ctypes, ctypes.wintypes as wt, os, tempfile, time
u32 = ctypes.WinDLL("user32", use_last_error=True)
shell32 = ctypes.WinDLL("shell32", use_last_error=True)
WM_KEYDOWN, WM_KEYUP, WM_CHAR, WM_GETTEXT, WM_SETTEXT, WM_NULL = 0x100, 0x101, 0x102, 0x0D, 0x0C, 0x0
EM_SETSEL = 0x00B1
SMTO = 3
SW_SHOWNOACTIVATE = 4


def cls(h):
    b = ctypes.create_unicode_buffer(64)
    u32.GetClassNameW(wt.HWND(h), b, 64)
    return b.value


def find(want, parent=0):
    out = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(h, l):
        if cls(h) == want:
            out.append(h)
        return True

    if parent:
        u32.EnumChildWindows(wt.HWND(parent), cb, 0)
    else:
        u32.EnumWindows(cb, 0)
    return out[0] if out else 0


def sm(h, m, w, l):
    r = ctypes.c_size_t()
    return u32.SendMessageTimeoutW(wt.HWND(h), m, w, l, SMTO, 1500, ctypes.byref(r))


def gt(h):
    b = ctypes.create_unicode_buffer(4096)
    sm(h, WM_GETTEXT, 4096, ctypes.cast(b, ctypes.c_void_p))
    return b.value


def st(h, s):
    sm(h, WM_SETTEXT, 0, ctypes.cast(ctypes.c_wchar_p(s), ctypes.c_void_p))


def lp(vk, down):
    v = 1 | (u32.MapVirtualKeyW(vk, 0) << 16)
    return v if down else (v | (1 << 30) | (1 << 31))


tmp = os.path.join(tempfile.gettempdir(), "qst_bs.txt")
open(tmp, "w").close()
os.system("taskkill /IM notepad.exe /F >nul 2>&1")
time.sleep(0.8)
shell32.ShellExecuteW(None, "open", "notepad.exe", f'"{tmp}"', None, SW_SHOWNOACTIVATE)
time.sleep(3.0)
top = find("Notepad")
re_h = find("RichEditD2DPT", top)
print("top=0x%X RichEditD2DPT=0x%X" % (top, re_h))


def case(label, seed, setup, seq):
    st(re_h, seed)
    time.sleep(0.2)
    if setup:
        setup()
    time.sleep(0.15)
    for vk, down in seq:
        u32.PostMessageW(wt.HWND(re_h), WM_KEYDOWN if down else WM_KEYUP, vk, lp(vk, down))
    u32.PostMessageW(wt.HWND(re_h), WM_NULL, 0, 0)
    time.sleep(0.45)
    print("  %-40s %r -> %r" % (label, seed, gt(re_h)))


sel_end = lambda: sm(re_h, EM_SETSEL, -1, -1)
sel_home = lambda: sm(re_h, EM_SETSEL, 0, 0)
case("KEYDOWN(BACK) 光标末尾", "X", sel_end, [(0x08, True), (0x08, False)])
case("KEYDOWN(BACK) 光标开头", "X", sel_home, [(0x08, True), (0x08, False)])
case("KEYDOWN(A) 光标末尾", "X", sel_end, [(0x41, True), (0x41, False)])
case("KEYDOWN(DELETE) 光标开头", "X", sel_home, [(0x2E, True), (0x2E, False)])
case("KEYDOWN(LEFT) 光标末尾", "X", sel_end, [(0x25, True), (0x25, False)])
