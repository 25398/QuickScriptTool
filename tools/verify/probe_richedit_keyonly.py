"""RichEditD2DPT「只发 KEYDOWN」是否够用 —— 决定双发修法。

用法: python probe_richedit_keyonly.py
"""
import ctypes
import ctypes.wintypes as wt
import os
import tempfile
import time

u32 = ctypes.WinDLL("user32", use_last_error=True)
shell32 = ctypes.WinDLL("shell32", use_last_error=True)

WM_KEYDOWN, WM_KEYUP, WM_CHAR, WM_GETTEXT, WM_SETTEXT, WM_NULL = 0x0100, 0x0101, 0x0102, 0x000D, 0x000C, 0x0000
SMTO = 0x0002 | 0x0001
SW_SHOWNOACTIVATE = 4
VK_RETURN, VK_BACK, VK_TAB, VK_LEFT, VK_DELETE, VK_SPACE = 0x0D, 0x08, 0x09, 0x25, 0x2E, 0x20


def cls(h):
    b = ctypes.create_unicode_buffer(64)
    u32.GetClassNameW(wt.HWND(h), b, 64)
    return b.value


def find_top():
    out = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(h, l):
        if cls(h) == "Notepad":
            out.append(h)
        return True

    u32.EnumWindows(cb, 0)
    return out[0] if out else 0


def find_desc(top, want):
    out = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(h, l):
        if cls(h) == want:
            out.append(h)
        return True

    u32.EnumChildWindows(wt.HWND(top), cb, 0)
    return out[0] if out else 0


def sm(hwnd, msg, wp, lp):
    res = ctypes.c_size_t()
    return u32.SendMessageTimeoutW(wt.HWND(hwnd), msg, wp, lp, SMTO, 1500, ctypes.byref(res))


def get_text(hwnd):
    buf = ctypes.create_unicode_buffer(4096)
    sm(hwnd, WM_GETTEXT, 4096, ctypes.cast(buf, ctypes.c_void_p))
    return buf.value


def set_text(hwnd, s):
    sm(hwnd, WM_SETTEXT, 0, ctypes.cast(ctypes.c_wchar_p(s), ctypes.c_void_p))


def lp(vk, down):
    scan = u32.MapVirtualKeyW(vk, 0)
    v = 1 | (scan << 16)
    if not down:
        v |= (1 << 30) | (1 << 31)
    return v


def case(re_hwnd, label, seed, seq):
    set_text(re_hwnd, seed)
    time.sleep(0.15)
    for vk, down in seq:
        u32.PostMessageW(wt.HWND(re_hwnd), WM_KEYDOWN if down else WM_KEYUP, vk, lp(vk, down))
    u32.PostMessageW(wt.HWND(re_hwnd), WM_NULL, 0, 0)
    time.sleep(0.45)
    print(f"  {label:<46} {seed!r} -> {get_text(re_hwnd)!r}")


def main():
    tmp = os.path.join(tempfile.gettempdir(), "qst_bg_probe3.txt")
    open(tmp, "w").close()
    os.system("taskkill /IM notepad.exe /F >nul 2>&1")
    time.sleep(0.8)
    shell32.ShellExecuteW(None, "open", "notepad.exe", f'"{tmp}"', None, SW_SHOWNOACTIVATE)
    time.sleep(3.0)
    top = find_top()
    re_hwnd = find_desc(top, "RichEditD2DPT")
    print(f"top=0x{top:X} RichEditD2DPT=0x{re_hwnd:X} fg={u32.GetForegroundWindow() == top}")
    print("只发 KEYDOWN+KEYUP（不发 WM_CHAR）：")
    case(re_hwnd, "A 字母", "", [(0x41, True), (0x41, False)])
    case(re_hwnd, "1 数字", "", [(0x31, True), (0x31, False)])
    case(re_hwnd, "空格", "", [(VK_SPACE, True), (VK_SPACE, False)])
    case(re_hwnd, "回车（应有换行）", "", [(VK_RETURN, True), (VK_RETURN, False)])
    case(re_hwnd, "退格（应删掉 X）", "X", [(VK_BACK, True), (VK_BACK, False)])
    case(re_hwnd, "删除键（应删掉 X）", "X", [(VK_DELETE, True), (VK_DELETE, False)])
    case(re_hwnd, "Tab", "", [(VK_TAB, True), (VK_TAB, False)])
    print("对照：只发 WM_CHAR（不发 KEYDOWN）")
    for label, ch in [("A 字母", "A"), ("回车", "\r")]:
        set_text(re_hwnd, "")
        time.sleep(0.15)
        u32.PostMessageW(wt.HWND(re_hwnd), WM_CHAR, ord(ch), 1)
        u32.PostMessageW(wt.HWND(re_hwnd), WM_NULL, 0, 0)
        time.sleep(0.45)
        print(f"  {label:<46} '' -> {get_text(re_hwnd)!r}")


if __name__ == "__main__":
    main()
