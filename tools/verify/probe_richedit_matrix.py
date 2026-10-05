"""隔离实验：RichEditD2DPT 到底吃哪种消息、会不会双发。

用法: python probe_richedit_matrix.py
"""
import ctypes
import ctypes.wintypes as wt
import os
import sys
import tempfile
import time

u32 = ctypes.WinDLL("user32", use_last_error=True)
shell32 = ctypes.WinDLL("shell32", use_last_error=True)

WM_KEYDOWN, WM_KEYUP, WM_CHAR, WM_GETTEXT, WM_SETTEXT, WM_NULL = 0x0100, 0x0101, 0x0102, 0x000D, 0x000C, 0x0000
SMTO = 0x0002 | 0x0001
SW_SHOWNOACTIVATE = 4


def cls(h):
    b = ctypes.create_unicode_buffer(64)
    u32.GetClassNameW(wt.HWND(h), b, 64)
    return b.value


def title(h):
    b = ctypes.create_unicode_buffer(256)
    u32.GetWindowTextW(wt.HWND(h), b, 256)
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


def vk_lparam(vk, down):
    scan = u32.MapVirtualKeyW(vk, 0)
    lp = 1 | (scan << 16)
    if not down:
        lp |= (1 << 30) | (1 << 31)
    return lp


def run_case(re_hwnd, label, msgs):
    set_text(re_hwnd, "")
    time.sleep(0.15)
    for msg, wp, lp in msgs:
        u32.PostMessageW(wt.HWND(re_hwnd), msg, wp, lp)
    u32.PostMessageW(wt.HWND(re_hwnd), WM_NULL, 0, 0)
    time.sleep(0.5)
    got = get_text(re_hwnd)
    print(f"  {label:<44} -> {got!r}")


def main():
    tmp = os.path.join(tempfile.gettempdir(), "qst_bg_probe2.txt")
    open(tmp, "w").close()
    os.system("taskkill /IM notepad.exe /F >nul 2>&1")
    time.sleep(0.8)
    shell32.ShellExecuteW(None, "open", "notepad.exe", f'"{tmp}"', None, SW_SHOWNOACTIVATE)
    time.sleep(3.0)
    top = find_top()
    if not top:
        print("启动记事本失败")
        return
    re_hwnd = find_desc(top, "RichEditD2DPT")
    ntb = find_desc(top, "NotepadTextBox")
    print(f"top=0x{top:X} NotepadTextBox=0x{ntb:X} RichEditD2DPT=0x{re_hwnd:X} "
          f"title={title(top)!r} fg={u32.GetForegroundWindow() == top}")
    print(f"父窗校验: RichEditD2DPT 的父 = 0x{u32.GetParent(wt.HWND(re_hwnd)):X} "
          f"({cls(u32.GetParent(wt.HWND(re_hwnd)))!r})")
    print("RichEditD2DPT 收到的消息矩阵：")
    vk = 0x41  # 'A'
    run_case(re_hwnd, "只 WM_CHAR 'a'",
             [(WM_CHAR, ord("a"), 1)])
    run_case(re_hwnd, "只 WM_KEYDOWN(A) [lParam=1]",
             [(WM_KEYDOWN, vk, 1)])
    run_case(re_hwnd, "只 WM_KEYDOWN(A) [正确扫描码]",
             [(WM_KEYDOWN, vk, vk_lparam(vk, True))])
    run_case(re_hwnd, "KEYDOWN(A)+CHAR(a) [引擎当前序列]",
             [(WM_KEYDOWN, vk, vk_lparam(vk, True)), (WM_CHAR, ord("a"), vk_lparam(vk, True))])
    run_case(re_hwnd, "KEYDOWN(A)+CHAR(a)+KEYUP",
             [(WM_KEYDOWN, vk, vk_lparam(vk, True)), (WM_CHAR, ord("a"), vk_lparam(vk, True)),
              (WM_KEYUP, vk, vk_lparam(vk, False))])
    run_case(re_hwnd, "CHAR(a)+KEYDOWN(A)",
             [(WM_CHAR, ord("a"), 1), (WM_KEYDOWN, vk, vk_lparam(vk, True))])
    print("NotepadTextBox（包装层）对照：")
    run_case(ntb, "只 WM_CHAR 'a'",
             [(WM_CHAR, ord("a"), 1)])
    print("顶层 Notepad 对照：")
    run_case(top, "只 WM_CHAR 'a'",
             [(WM_CHAR, ord("a"), 1)])


if __name__ == "__main__":
    main()
