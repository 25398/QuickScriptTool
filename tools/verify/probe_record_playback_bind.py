#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""探针：录制产出的 windowMode 配置在「标题变了」之后还能不能绑到目标窗口。

背景（用户反馈）：
    后台窗口模式 + 自动识别 录制的脚本，回放时「不操作后台」——按键/点击到不了目标。

怀疑根因：
    录制保存时把**录制瞬间的窗口标题**写进了 windowName（engine_host_window.h:8305
    `wm.windowName = wmTgt.windowTitle`），回放端 BuildTargetQuery 对
    UseEditorWindowClass 分支把它按 " - " 截成 stem 当 titleContains
    （window_target.cpp:491），而 EnumWindowsOnDesktopProc 里标题匹配是**硬门**
    （window_target.cpp:327，排在类名匹配之前）。

    于是「同一程序、同一窗口类、标题变了」（换文档/换场景/换标签页）⇒ 枚举不到 ⇒ 绑不到。

实现说明：
    不依赖记事本（Win11 会进程交接，PID 变），改用**本探针自建的顶层窗口**，
    类名 `QstProbeWnd`、标题完全可控，作为「目标程序」的替身。

用法：
    python tools/verify/probe_record_playback_bind.py
输出：每步一行 KEY=VALUE，最后 BIND_RESULT=... / VERDICT=...
"""

import ctypes
import ctypes.wintypes as wt
import sys
import threading
import time

u32 = ctypes.WinDLL("user32", use_last_error=True)
k32 = ctypes.WinDLL("kernel32", use_last_error=True)
g32 = ctypes.WinDLL("gdi32", use_last_error=True)

# 显式声明原型，避免 64 位下默认 int 返回值截断指针。
u32.DefWindowProcW.restype = ctypes.c_ssize_t
u32.DefWindowProcW.argtypes = [wt.HWND, ctypes.c_uint, ctypes.c_size_t, ctypes.c_ssize_t]
u32.SetWindowTextW.restype = wt.BOOL
u32.SetWindowTextW.argtypes = [wt.HWND, wt.LPCWSTR]

EnumWindowsProc = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
# ⚠ 必须用 c_ssize_t / c_size_t：LPARAM 在 64 位下是有符号的，
# 用 c_longlong 会让 DefWindowProcW 的 (WPARAM, LPARAM) 传参溢出。
WNDPROC = ctypes.WINFUNCTYPE(ctypes.c_ssize_t, wt.HWND, ctypes.c_uint,
                             ctypes.c_size_t, ctypes.c_ssize_t)

GWL_EXSTYLE = -20
GWL_STYLE = -16
WS_EX_TOOLWINDOW = 0x00000080
WS_EX_APPWINDOW = 0x00040000
WS_CHILD = 0x40000000
WS_OVERLAPPEDWINDOW = 0x00CF0000
WS_VISIBLE = 0x10000000
CW_USEDEFAULT = 0x80000000
SW_SHOWNOACTIVATE = 4
GW_OWNER = 4
WM_DESTROY = 0x0002
WM_CLOSE = 0x0010


class WNDCLASSW(ctypes.Structure):
    _fields_ = [
        ("style", wt.UINT),
        ("lpfnWndProc", WNDPROC),
        ("cbClsExtra", ctypes.c_int),
        ("cbWndExtra", ctypes.c_int),
        ("hInstance", wt.HINSTANCE),
        ("hIcon", wt.HICON),
        ("hCursor", wt.HANDLE),
        ("hbrBackground", wt.HBRUSH),
        ("lpszMenuName", wt.LPCWSTR),
        ("lpszClassName", wt.LPCWSTR),
    ]


_wndproc_ref = None
_hwnd_box = [0]
_msg_ready = threading.Event()


def _wndproc(hwnd, msg, wp, lp):
    if msg == WM_CLOSE:
        u32.DestroyWindow(hwnd)
        return 0
    if msg == WM_DESTROY:
        u32.PostQuitMessage(0)
        return 0
    return u32.DefWindowProcW(hwnd, msg, wp, lp)


def _pump():
    global _wndproc_ref
    hinst = k32.GetModuleHandleW(None)
    _wndproc_ref = WNDPROC(_wndproc)
    wc = WNDCLASSW()
    wc.lpfnWndProc = _wndproc_ref
    wc.hInstance = hinst
    wc.lpszClassName = "QstProbeWnd"
    if not u32.RegisterClassW(ctypes.byref(wc)):
        err = ctypes.get_last_error()
        # 1390 = 类已存在，可忽略
        if err != 1390:
            _msg_ready.set()
            return
    h = u32.CreateWindowExW(
        0, "QstProbeWnd", "QstProbeWnd", WS_OVERLAPPEDWINDOW,
        120, 120, 900, 640, None, None, hinst, None,
    )
    _hwnd_box[0] = h
    u32.ShowWindow(wt.HWND(h), SW_SHOWNOACTIVATE)
    _msg_ready.set()
    msg = wt.MSG()
    while u32.GetMessageW(ctypes.byref(msg), None, 0, 0) > 0:
        u32.TranslateMessage(ctypes.byref(msg))
        u32.DispatchMessageW(ctypes.byref(msg))


def get_class(hwnd):
    buf = ctypes.create_unicode_buffer(256)
    u32.GetClassNameW(wt.HWND(hwnd), buf, 256)
    return buf.value


def get_title(hwnd):
    buf = ctypes.create_unicode_buffer(512)
    u32.GetWindowTextW(wt.HWND(hwnd), buf, 512)
    return buf.value


def get_pid(hwnd):
    pid = wt.DWORD(0)
    u32.GetWindowThreadProcessId(wt.HWND(hwnd), ctypes.byref(pid))
    return pid.value


def to_lower(s):
    return s.lower()


def strip_browser_noise(t):
    t = t.strip()
    while len(t) > 3 and t.endswith(")") and "(" in t:
        i = t.rfind("(")
        seg = t[i + 1:-1]
        if seg.isdigit() or seg.replace("+", "").isdigit():
            t = t[:i].strip()
            continue
        break
    return t


def title_matches(title, needle):
    """等价复刻 window_mode/window_target.cpp:192 TitleMatches。"""
    if not needle:
        return True
    tl, nl = to_lower(title), to_lower(needle)
    if nl in tl:
        return True
    ct, cn = to_lower(strip_browser_noise(title)), to_lower(strip_browser_noise(needle))
    if not cn:
        return True
    if cn in ct:
        return True
    if ct and ct in cn:
        return True
    return False


def class_matches(actual, expected):
    if not expected:
        return True
    return to_lower(actual) == to_lower(expected)


def strip_stem(title):
    """等价复刻 window_target.cpp:475-490 的 stem 提取（" - " 截断 + 去 无标题）。"""
    stem = title.strip()
    dash = stem.find(" - ")
    if dash != -1:
        stem = stem[:dash].strip()
    stem = stem.lstrip("* ").strip()
    if stem == "无标题" or stem.lower() == "untitled":
        return ""
    return stem


def is_likely_main(hwnd, relaxed=False):
    if not u32.IsWindow(wt.HWND(hwnd)):
        return False
    if not relaxed and not u32.IsWindowVisible(wt.HWND(hwnd)):
        return False
    if not relaxed and u32.GetWindow(wt.HWND(hwnd), GW_OWNER):
        return False
    style = u32.GetWindowLongW(wt.HWND(hwnd), GWL_STYLE)
    if style & WS_CHILD:
        return False
    ex = u32.GetWindowLongW(wt.HWND(hwnd), GWL_EXSTYLE)
    if (ex & WS_EX_TOOLWINDOW) and not (ex & WS_EX_APPWINDOW):
        return False
    return True


def enum_matching_windows(pid, class_name, title_contains):
    """等价复刻 EnumWindowsOnDesktopProc 判据顺序：
    IsLikelyMainWindow -> pid -> TitleMatches -> ClassMatches。"""
    hits = []

    @EnumWindowsProc
    def cb(hwnd, _):
        if not is_likely_main(hwnd):
            return True
        if get_pid(hwnd) != pid:
            return True
        if not title_matches(get_title(hwnd), title_contains):
            return True
        if not class_matches(get_class(hwnd), class_name):
            return True
        hits.append(hwnd)
        return True

    u32.EnumWindows(cb, 0)
    return hits


def set_title(hwnd, text):
    u32.SetWindowTextW(wt.HWND(hwnd), text)


def main():
    print("PROBE=probe_record_playback_bind")
    t = threading.Thread(target=_pump, daemon=True)
    t.start()
    if not _msg_ready.wait(5.0) or not _hwnd_box[0]:
        print("VERDICT=SKIP_NO_WINDOW")
        return 2

    h = _hwnd_box[0]
    pid = get_pid(h)
    cls = get_class(h)
    print("PROBE_PID=%d" % pid)
    print("TOP_CLASS=%s" % cls)
    time.sleep(0.3)

    # ---- 第 1 步：模拟「录制瞬间」的标题 T1 ----
    t1 = "QST_PROBE_REC_TITLE.txt - 记事本"
    set_title(h, t1)
    time.sleep(0.3)
    rec_title = get_title(h)
    print("RECORD_TITLE(raw)=%s" % rec_title)

    # ---- 录制保存路径的字段赋值（engine_host_window.h:8302-8305）----
    cfg_window_name = rec_title      # wm.windowName = wmTgt.windowTitle
    cfg_class = cls                  # wm.windowClassName = wmTgt.windowClassName
    cfg_title_contains = strip_stem(cfg_window_name)   # BuildTargetQuery 的 stem
    print("CFG_windowName=%s" % cfg_window_name)
    print("CFG_windowClassName=%s" % cfg_class)
    print("CFG_titleContains=%s" % (cfg_title_contains if cfg_title_contains else "<empty>"))

    # ---- 第 2 步：标题没变 → 应该能绑到 ----
    hits_same = enum_matching_windows(pid, cfg_class, cfg_title_contains)
    print("ENUM_TITLE_UNCHANGED=%d" % len(hits_same))

    # ---- 第 3 步：回放时标题已变（换文档 / 换标签页 / 换存档）----
    t2 = "QST_PROBE_PLAY_TITLE.txt - 记事本"
    set_title(h, t2)
    time.sleep(0.3)
    print("PLAY_TITLE(raw)=%s" % get_title(h))
    hits_changed = enum_matching_windows(pid, cfg_class, cfg_title_contains)
    print("ENUM_TITLE_CHANGED=%d" % len(hits_changed))

    # ---- 第 4 步：游戏式「场景名变化」 ----
    t3 = "某游戏 - 第 3 章 关卡二"
    set_title(h, t3)
    time.sleep(0.3)
    print("SCENE_TITLE(raw)=%s" % get_title(h))
    hits_scene = enum_matching_windows(pid, cfg_class, cfg_title_contains)
    print("ENUM_SCENE_CHANGED=%d" % len(hits_scene))

    # ---- 第 5 步：反证——若去掉 titleContains（只用类名+pid）能否找到 ----
    hits_nofilter = enum_matching_windows(pid, cfg_class, "")
    print("ENUM_TITLE_FILTER_OFF=%d" % len(hits_nofilter))

    if len(hits_same) == 0:
        verdict = "INCONCLUSIVE"
        print("BIND_RESULT=INCONCLUSIVE_NO_WINDOW_FOUND")
    elif len(hits_changed) == 0 and len(hits_scene) == 0:
        verdict = "ROOT_CAUSE_CONFIRMED"
        print("BIND_RESULT=TITLE_LOCKED_BLOCKS_REBIND")
    elif len(hits_changed) > 0:
        verdict = "NOT_REPRODUCED"
        print("BIND_RESULT=TITLE_TOLERANT")
    else:
        verdict = "PARTIAL"
        print("BIND_RESULT=PARTIAL")

    print("COUNTERFACTUAL_TITLE_FILTER_OFF=%d" % len(hits_nofilter))
    print("VERDICT=%s" % verdict)
    try:
        u32.PostMessageW(wt.HWND(h), WM_CLOSE, 0, 0)
    except Exception:
        pass
    return 0 if verdict == "ROOT_CAUSE_CONFIRMED" else 1


if __name__ == "__main__":
    sys.exit(main())
