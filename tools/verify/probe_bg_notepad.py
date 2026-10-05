"""后台窗口模式按键投递探针（取证用，不进任何自检清单）。

目的：查清「后台 PostMessage 打按键」对现代记事本（WinUI3 商店版）到底哪个 HWND 才吃键。

用法：
    python probe_bg_notepad.py launch                  # 以「不激活」方式起一个记事本（临时空文件）
    python probe_bg_notepad.py hier                    # 递归打印窗口层级（含父子关系）
    python probe_bg_notepad.py read                    # 读候选窗口文本 + 顶层标题
    python probe_bg_notepad.py post <hwnd> <ch> [mode] # mode=key(默认, DOWN+CHAR+UP) / char(只 CHAR)
    python probe_bg_notepad.py exp <hwnd>              # 干净实例 + 只投 CHAR，报告文本增量
    python probe_bg_notepad.py close                   # 关掉全部记事本
"""
import ctypes
import ctypes.wintypes as wt
import os
import sys
import tempfile
import time

u32 = ctypes.WinDLL("user32", use_last_error=True)
shell32 = ctypes.WinDLL("shell32", use_last_error=True)

WM_KEYDOWN = 0x0100
WM_KEYUP = 0x0101
WM_CHAR = 0x0102
WM_GETTEXT = 0x000D
WM_NULL = 0x0000
SMTO_ABORTIFHUNG = 0x0002
SMTO_BLOCK = 0x0001
SW_SHOWNOACTIVATE = 4

TMPFILE = os.path.join(tempfile.gettempdir(), "qst_bg_probe.txt")


def get_class(hwnd):
    buf = ctypes.create_unicode_buffer(256)
    u32.GetClassNameW(wt.HWND(hwnd), buf, 256)
    return buf.value


def get_text(hwnd):
    buf = ctypes.create_unicode_buffer(1024)
    u32.GetWindowTextW(wt.HWND(hwnd), buf, 1024)
    return buf.value


def get_rect(hwnd):
    rc = wt.RECT()
    u32.GetClientRect(wt.HWND(hwnd), ctypes.byref(rc))
    return rc


def get_pid(hwnd):
    pid = wt.DWORD()
    u32.GetWindowThreadProcessId(wt.HWND(hwnd), ctypes.byref(pid))
    return pid.value


def enum_children(parent):
    out = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(hwnd, lp):
        out.append(hwnd)
        return True

    u32.EnumChildWindows(wt.HWND(parent), cb, 0)
    return out


def enum_tops():
    out = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(hwnd, lp):
        out.append(hwnd)
        return True

    u32.EnumWindows(cb, 0)
    return out


def find_notepad_tops():
    return [(h, get_class(h), get_text(h)) for h in enum_tops() if get_class(h) == "Notepad"]


def notepad_top():
    tops = find_notepad_tops()
    return tops[0][0] if tops else 0


def wait_notepad(timeout=10.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        tops = find_notepad_tops()
        if tops:
            return tops
        time.sleep(0.2)
    return []


def read_hwnd_text(hwnd):
    buf = ctypes.create_unicode_buffer(4096)
    res = ctypes.c_size_t()
    ok = u32.SendMessageTimeoutW(wt.HWND(hwnd), WM_GETTEXT, 4096,
                                 ctypes.cast(buf, ctypes.c_void_p),
                                 SMTO_ABORTIFHUNG | SMTO_BLOCK, 1500, ctypes.byref(res))
    return ok, buf.value


def rich_edit(top):
    for c in enum_children(top):
        if get_class(c) == "RichEditD2DPT":
            return c
    return 0


def dump_hier(top, depth=0):
    rc = get_rect(top)
    print("  " * depth + f"0x{top:X} {get_class(top)!r} "
          f"client={rc.right-rc.left}x{rc.bottom-rc.top}")
    for c in enum_children(top):
        # EnumChildWindows 是递归的：只画直接子窗，避免重复
        if u32.GetParent(wt.HWND(c)) == top:
            dump_hier(c, depth + 1)


def close_all_notepad():
    os.system("taskkill /IM notepad.exe /F >nul 2>&1")
    time.sleep(0.8)


def launch_notepad(path=None):
    if path:
        open(path, "w", encoding="utf-8").close()
    close_all_notepad()
    arg = f'"{path}"' if path else None
    shell32.ShellExecuteW(None, "open", "notepad.exe", arg, None, SW_SHOWNOACTIVATE)
    tops = wait_notepad()
    if not tops:
        return 0
    time.sleep(1.2)  # 等 WinUI 把 RichEditD2DPT 建出来
    tops = find_notepad_tops()
    h = tops[0][0]
    print(f"记事本 top=0x{h:X} pid={get_pid(h)} title={get_text(h)!r} "
          f"fg=0x{u32.GetForegroundWindow():X}")
    return h


def post(hwnd, ch, mode):
    vk = u32.VkKeyScanW(ctypes.c_wchar(ch)) & 0xFF or ord(ch.upper())
    if mode == "char":
        u32.PostMessageW(wt.HWND(hwnd), WM_CHAR, ord(ch), 1)
    else:
        u32.PostMessageW(wt.HWND(hwnd), WM_KEYDOWN, vk, 1)
        u32.PostMessageW(wt.HWND(hwnd), WM_CHAR, ord(ch), 1)
        u32.PostMessageW(wt.HWND(hwnd), WM_KEYUP, vk, 0xC0000001)
    u32.PostMessageW(wt.HWND(hwnd), WM_NULL, 0, 0)
    return vk


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return
    cmd = sys.argv[1]
    if cmd == "close":
        close_all_notepad()
        print("已关闭全部记事本")
        return
    if cmd == "launch":
        launch_notepad()
        return
    if cmd == "hier":
        top = notepad_top()
        if not top:
            print("没有记事本")
            return
        dump_hier(top)
        return
    if cmd == "read":
        top = notepad_top()
        if not top:
            print("没有记事本")
            return
        re_hwnd = rich_edit(top)
        ok, txt = read_hwnd_text(re_hwnd) if re_hwnd else (0, "")
        print(f"title={get_text(top)!r} fg={u32.GetForegroundWindow() == top} "
              f"RichEditD2DPT@0x{re_hwnd:X} sendOk={ok} text={txt!r}")
        return
    if cmd == "post":
        top = notepad_top()
        hwnd = int(sys.argv[2], 0)
        ch = sys.argv[3] if len(sys.argv) > 3 else "a"
        mode = sys.argv[4] if len(sys.argv) > 4 else "key"
        re_hwnd = rich_edit(top)
        _, before = read_hwnd_text(re_hwnd)
        vk = post(hwnd, ch, mode)
        time.sleep(0.7)
        _, after = read_hwnd_text(re_hwnd)
        print(f"投递 vk=0x{vk:02X} mode={mode} -> 0x{hwnd:X} ({get_class(hwnd)!r})")
        print(f"  RichEdit 文本: {before!r} -> {after!r}")
        print(f"  结果: {'进去了' if after != before else '没进去'}")
        return
    if cmd == "exp":
        hwnd = int(sys.argv[2], 0)
        ch = sys.argv[3] if len(sys.argv) > 3 else "a"
        mode = sys.argv[4] if len(sys.argv) > 4 else "char"
        top = launch_notepad(TMPFILE)
        if not top:
            print("启动失败")
            return
        target = hwnd if hwnd else top
        re_hwnd = rich_edit(top)
        _, before = read_hwnd_text(re_hwnd)
        vk = post(target, ch, mode)
        time.sleep(0.9)
        _, after = read_hwnd_text(re_hwnd)
        print(f"  目标 0x{target:X} ({get_class(target)!r}) mode={mode} ch={ch!r} vk=0x{vk:02X}")
        print(f"  RichEdit 文本: {before!r} -> {after!r}")
        print(f"  结果: {'进去了' if after != before else '没进去'}")
        return
    print("未知命令", cmd)


if __name__ == "__main__":
    main()
