// Included inside fake_focus_dll.cpp anonymous namespace.
// UnityPlayer polls Raw Input (PeekMessage WM_INPUT / GetRawInputBuffer) instead of
// (or in addition to) GetCursorPos / GetAsyncKeyState.

constexpr ULONG_PTR kFakeRawHandleValue = 0x51465257ull;  // 'QFRW'
inline HRAWINPUT FakeRawHandle() {
    return reinterpret_cast<HRAWINPUT>(kFakeRawHandleValue);
}

fakefocus::InlineHook g_hookRawData{};
fakefocus::InlineHook g_hookRawBuffer{};
fakefocus::InlineHook g_hookPeekW{};
fakefocus::InlineHook g_hookPeekA{};
fakefocus::InlineHook g_hookGetMsgW{};
fakefocus::InlineHook g_hookGetMsgA{};

RAWINPUT g_rawPending{};
bool g_rawValid = false;
uint32_t g_rawLastSeq = 0;
int g_rawLastX = 0;
int g_rawLastY = 0;
uint8_t g_rawLastButtons = 0;
bool g_rawHaveLast = false;

/// 取出一格**待投递的滚轮**（宿主写进共享内存，这里按 Raw Input 投给目标）。
///
/// ⚠⚠ 为什么必须走这里（2026-09-30 真机报障「后台窗口模式滚动不能正常滚动」）：
///   `WM_MOUSEWHEEL` 只有**消息驱动**的应用会读；而假焦点服务的对象恰恰是
///   Unity/UE/GLFW 这类**用 Raw Input 读输入**的游戏 —— 对它们 PostMessage
///   等于什么都没发生，且不会有任何报错。滚轮因此必须出现在我们本来就伪造的
///   `WM_INPUT` 流里，与光标/按键同一条路。
///
/// @param outHoriz true = 横向滚轮
/// @param outPositive true = 向上/向左
/// @param outNotches 这一格代表的格数（≥1）
/// @return false = 队列里没有新滚轮
bool TakePendingRawWheel(bool& outHoriz, bool& outPositive, int& outNotches) {
    const fakefocus::SoftInputState* st = SoftState();
    if (!st) return false;
    const uint32_t w = st->wheelWrite;
    uint32_t r = st->wheelRead;
    if (r >= w) return false;
    // 宿主写太快时的环形覆盖：追到队列容量以内，别去读已经被覆盖的槽。
    if (w - r > fakefocus::kSoftKeyEventCap) r = w - fakefocus::kSoftKeyEventCap;
    const fakefocus::SoftKeyEvent e = st->wheelEvents[r % fakefocus::kSoftKeyEventCap];
    // 先推游标再消费：中途被抢占也只是"这一格重投一次"，不会永久卡住队列。
    // ⚠ 这里**不**通过 `const_cast` 写回（st 是 const 视图）—— 写游标放到下面
    //   `ConsumePendingRawWheel()` 里，用非 const 的 `SoftStateWritable()`。
    outHoriz = (e.vk == 0xFD);
    outPositive = (e.down != 0);
    outNotches = e.pad ? static_cast<int>(e.pad) : 1;
    if (outNotches < 1) outNotches = 1;
    return true;
}

/// 推进滚轮读游标（与 `TakePendingRawWheel` 配对，**成功投递之后**才调）。
void ConsumePendingRawWheel() {
    if (!g_softWritable || !g_softView) return;
    if (!fakefocus::SoftInputStateLooksValid(g_softView)) return;
    const uint32_t w = g_softView->wheelWrite;
    uint32_t r = g_softView->wheelRead;
    if (r >= w) return;
    if (w - r > fakefocus::kSoftKeyEventCap) r = w - fakefocus::kSoftKeyEventCap;
    g_softView->wheelRead = r + 1;
}

/// 把一格滚轮编码成 `RAWMOUSE` 的 `usButtonFlags` / `usButtonData`。
///
/// ⚠ `usButtonData` 声明成 `USHORT`（无符号），但语义是**有符号**的格数 ×
///   `WHEEL_DELTA`（MSDN：`usButtonData` 为滚轮增量）。向下滚是负数，
///   必须显式 `static_cast<SHORT>` 再转回无符号位型 —— 直接赋负数会被
///   隐式转换告警/截断（这正是宿主侧 `wParam` 高位那个坑的同一形态）。
void FillWheelRawFlags(int notches, bool horizontal, bool positive, USHORT& flags, USHORT& data) {
    if (notches < 1) notches = 1;
    if (notches > windowmode::kMaxWheelNotchesPerEvent) {
        notches = windowmode::kMaxWheelNotchesPerEvent;
    }
    const int delta = (positive ? 1 : -1) * notches * windowmode::kWheelNotchDelta;
    flags = horizontal ? RI_MOUSE_HWHEEL : RI_MOUSE_WHEEL;
    data = static_cast<USHORT>(static_cast<SHORT>(delta));
}

uint8_t SoftMouseButtons() {
    const fakefocus::SoftInputState* st = SoftState();
    if (!st) return 0;
    uint8_t b = 0;
    if (st->down[VK_LBUTTON]) b |= 1u;
    if (st->down[VK_RBUTTON]) b |= 2u;
    if (st->down[VK_MBUTTON]) b |= 4u;
    return b;
}

bool FillPendingRawInput() {
    if (g_rawValid) return true;
    const fakefocus::SoftInputState* st = SoftState();
    if (!st || !(st->flags & fakefocus::kSoftFlagCursorValid)) return false;

    const int x = st->cursorScreenX;
    const int y = st->cursorScreenY;
    const uint8_t buttons = SoftMouseButtons();
    if (!g_rawHaveLast) {
        g_rawLastX = x;
        g_rawLastY = y;
        g_rawHaveLast = true;
    }

    // ★★ 滚轮优先（2026-09-30）：它必须**独占一条** `WM_INPUT`。
    //   把滚轮和光标/按键合并在同一条里，靠 `usButtonData` 携带增量是可行的，
    //   但 `RAWMOUSE` 一次只发一个事件的语义下，合并会让"滚了 3 格"变成
    //   "移动 + 滚 3 格"这种说不清的组合（部分引擎只取位移，滚轮被吞）。
    //   ⇒ 一格一条，与真实硬件的中断粒度一致。
    bool wh = false;
    bool wp = false;
    int wn = 1;
    if (TakePendingRawWheel(wh, wp, wn)) {
        RAWINPUT ri{};
        ri.header.dwType = RIM_TYPEMOUSE;
        ri.header.dwSize = static_cast<DWORD>(sizeof(RAWINPUT));
        ri.data.mouse.usFlags = 0;
        ri.data.mouse.lLastX = 0;
        ri.data.mouse.lLastY = 0;
        FillWheelRawFlags(wn, wh, wp, ri.data.mouse.usButtonFlags,
            ri.data.mouse.usButtonData);
        g_rawPending = ri;
        g_rawValid = true;
        // ⚠ 不动 g_rawLastX/Y/buttons：滚轮事件不代表光标或按键状态发生变化，
        //   改了它们会让下一次光标事件算出错误的相对位移。
        ConsumePendingRawWheel();
        return true;
    }

    const bool moved = (x != g_rawLastX || y != g_rawLastY);
    const bool btnChanged = (buttons != g_rawLastButtons);
    const bool seqChanged = (st->seq != g_rawLastSeq);
    if (!moved && !btnChanged && !seqChanged) return false;

    RAWINPUT ri{};
    ri.header.dwType = RIM_TYPEMOUSE;
    ri.header.dwSize = static_cast<DWORD>(sizeof(RAWINPUT));
    ri.header.hDevice = nullptr;
    ri.header.wParam = 0;
    // GLFW/Unity 3D 在光标锁定时按相对位移读 Raw Input；绝对 0~65535 会把 1px
    // 相对移动量化丢精度，暂停菜单点不中、视角乱跳。
    ri.data.mouse.usFlags = 0;
    ri.data.mouse.lLastX = x - g_rawLastX;
    ri.data.mouse.lLastY = y - g_rawLastY;

    USHORT flags = 0;
    if ((buttons & 1u) && !(g_rawLastButtons & 1u)) flags |= RI_MOUSE_LEFT_BUTTON_DOWN;
    if (!(buttons & 1u) && (g_rawLastButtons & 1u)) flags |= RI_MOUSE_LEFT_BUTTON_UP;
    if ((buttons & 2u) && !(g_rawLastButtons & 2u)) flags |= RI_MOUSE_RIGHT_BUTTON_DOWN;
    if (!(buttons & 2u) && (g_rawLastButtons & 2u)) flags |= RI_MOUSE_RIGHT_BUTTON_UP;
    if ((buttons & 4u) && !(g_rawLastButtons & 4u)) flags |= RI_MOUSE_MIDDLE_BUTTON_DOWN;
    if (!(buttons & 4u) && (g_rawLastButtons & 4u)) flags |= RI_MOUSE_MIDDLE_BUTTON_UP;
    ri.data.mouse.usButtonFlags = flags;

    g_rawPending = ri;
    g_rawValid = true;
    g_rawLastX = x;
    g_rawLastY = y;
    g_rawLastButtons = buttons;
    g_rawLastSeq = st->seq;
    return true;
}

void ConsumePendingRawInput() {
    g_rawValid = false;
}

void ResetRawInputState() {
    g_rawValid = false;
    g_rawHaveLast = false;
    g_rawLastSeq = 0;
    g_rawLastX = 0;
    g_rawLastY = 0;
    g_rawLastButtons = 0;
    std::memset(&g_rawPending, 0, sizeof(g_rawPending));
}

bool MessageFilterIncludesWmInput(UINT min, UINT max) {
    if (min == 0 && max == 0) return true;
    return WM_INPUT >= min && WM_INPUT <= max;
}

bool FillFakeInputMsg(LPMSG lpMsg, HWND hWnd) {
    if (!lpMsg || !FillPendingRawInput()) return false;
    HWND top = g_targetTop.load(std::memory_order_relaxed);
    if (hWnd && top && hWnd != top && !IsChild(top, hWnd)) return false;
    lpMsg->hwnd = top ? top : hWnd;
    lpMsg->message = WM_INPUT;
    lpMsg->wParam = RIM_INPUT;
    lpMsg->lParam = reinterpret_cast<LPARAM>(FakeRawHandle());
    lpMsg->time = GetTickCount();
    lpMsg->pt.x = g_rawLastX;
    lpMsg->pt.y = g_rawLastY;
    return true;
}

struct PeekCallCtx {
    LPMSG lpMsg = nullptr;
    HWND hWnd = nullptr;
    UINT min = 0;
    UINT max = 0;
    UINT remove = 0;
    BOOL unicode = TRUE;
    BOOL ok = FALSE;
};
void* CallOrigPeek(void* tramp, void* raw) {
    auto* ctx = static_cast<PeekCallCtx*>(raw);
    using Fn = BOOL(WINAPI*)(LPMSG, HWND, UINT, UINT, UINT);
    auto fn = reinterpret_cast<Fn>(tramp);
    ctx->ok = ctx->unicode
        ? fn(ctx->lpMsg, ctx->hWnd, ctx->min, ctx->max, ctx->remove)
        : fn(ctx->lpMsg, ctx->hWnd, ctx->min, ctx->max, ctx->remove);
    return nullptr;
}
BOOL CallOriginalPeek(LPMSG lpMsg, HWND hWnd, UINT min, UINT max, UINT remove, bool unicode) {
    PeekCallCtx ctx{lpMsg, hWnd, min, max, remove, unicode ? TRUE : FALSE, FALSE};
    fakefocus::CallThroughOriginal(unicode ? g_hookPeekW : g_hookPeekA, &CallOrigPeek, &ctx, nullptr);
    return ctx.ok;
}

struct GetMsgCallCtx {
    LPMSG lpMsg = nullptr;
    HWND hWnd = nullptr;
    UINT min = 0;
    UINT max = 0;
    BOOL unicode = TRUE;
    BOOL ok = FALSE;
};
void* CallOrigGetMsg(void* tramp, void* raw) {
    auto* ctx = static_cast<GetMsgCallCtx*>(raw);
    using Fn = BOOL(WINAPI*)(LPMSG, HWND, UINT, UINT);
    auto fn = reinterpret_cast<Fn>(tramp);
    ctx->ok = ctx->unicode
        ? fn(ctx->lpMsg, ctx->hWnd, ctx->min, ctx->max)
        : fn(ctx->lpMsg, ctx->hWnd, ctx->min, ctx->max);
    return nullptr;
}
BOOL CallOriginalGetMessage(LPMSG lpMsg, HWND hWnd, UINT min, UINT max, bool unicode) {
    GetMsgCallCtx ctx{lpMsg, hWnd, min, max, unicode ? TRUE : FALSE, FALSE};
    fakefocus::CallThroughOriginal(unicode ? g_hookGetMsgW : g_hookGetMsgA, &CallOrigGetMsg, &ctx, nullptr);
    return ctx.ok;
}

struct RawDataCallCtx {
    HRAWINPUT hRaw = nullptr;
    UINT cmd = 0;
    LPVOID pData = nullptr;
    PUINT pcbSize = nullptr;
    UINT cbHeader = 0;
    UINT result = 0;
};
void* CallOrigRawData(void* tramp, void* raw) {
    auto* ctx = static_cast<RawDataCallCtx*>(raw);
    using Fn = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
    ctx->result = reinterpret_cast<Fn>(tramp)(ctx->hRaw, ctx->cmd, ctx->pData, ctx->pcbSize, ctx->cbHeader);
    return nullptr;
}
UINT CallOriginalRawData(HRAWINPUT hRaw, UINT cmd, LPVOID pData, PUINT pcbSize, UINT cbHeader) {
    RawDataCallCtx ctx{hRaw, cmd, pData, pcbSize, cbHeader, 0};
    fakefocus::CallThroughOriginal(g_hookRawData, &CallOrigRawData, &ctx, nullptr);
    return ctx.result;
}

struct RawBufCallCtx {
    PRAWINPUT pData = nullptr;
    PUINT pcbSize = nullptr;
    UINT cbHeader = 0;
    UINT result = 0;
};
void* CallOrigRawBuf(void* tramp, void* raw) {
    auto* ctx = static_cast<RawBufCallCtx*>(raw);
    using Fn = UINT(WINAPI*)(PRAWINPUT, PUINT, UINT);
    ctx->result = reinterpret_cast<Fn>(tramp)(ctx->pData, ctx->pcbSize, ctx->cbHeader);
    return nullptr;
}
UINT CallOriginalRawBuffer(PRAWINPUT pData, PUINT pcbSize, UINT cbHeader) {
    RawBufCallCtx ctx{pData, pcbSize, cbHeader, 0};
    fakefocus::CallThroughOriginal(g_hookRawBuffer, &CallOrigRawBuf, &ctx, nullptr);
    return ctx.result;
}

bool SoftCursorBlocksPhysicalMouse() {
    if (g_electronSafe) return false;
    const fakefocus::SoftInputState* st = SoftState();
    if (!st || (st->flags & fakefocus::kSoftFlagPostKeyEvents)) return false;
    return (st->flags & fakefocus::kSoftFlagCursorValid) != 0;
}

bool IsFakeRawInputMsg(const MSG* lpMsg) {
    return lpMsg
        && lpMsg->message == WM_INPUT
        && lpMsg->lParam == reinterpret_cast<LPARAM>(FakeRawHandle());
}

BOOL HookPeekMessage(LPMSG lpMsg, HWND hWnd, UINT min, UINT max, UINT remove, bool unicode) {
    // Electron：只在窗口 PID 用 PostMessage 队列灌键；此处禁止伪造 Peek 消息（会崩 QQNT）。
    DrainSoftKeyEventsPost();
    const bool swallowPhysical = SoftCursorBlocksPhysicalMouse();
    for (int skip = 0; skip < 32; ++skip) {
        const BOOL got = CallOriginalPeek(lpMsg, hWnd, min, max, remove, unicode);
        if (!got) break;
        // 输入路径体检（2026-10-04）：通用泵钩子原来**一次都不计数** —— 只有冒险岛那套
        // `Hook_Maple*` 才数 ⇒ Unity / UE / GLFW / 模拟器目标的 hitPump / msgInput / msgKey
        // **恒为 0**，宿主导出的体检报告全 0，于是分不清「游戏不走这些入口」和
        // 「我们根本没在数」。这里补上，判读见 window_mode_requirements.h §22。
        if (lpMsg) MapleNotePumpMessage(lpMsg->message);
        // 丢掉用户真鼠标的 WM_INPUT（RIDEV_INPUTSINK / 前台漏消息），避免 3D 视角被本机光标带着走。
        // 我们自己 Post 的假 WM_INPUT（FakeRawHandle）必须放行。
        if (swallowPhysical && lpMsg && lpMsg->message == WM_INPUT
            && !IsFakeRawInputMsg(lpMsg)) {
            if ((remove & PM_REMOVE) == 0) {
                if (FillFakeInputMsg(lpMsg, hWnd)) return TRUE;
                lpMsg->message = WM_NULL;
                return TRUE;
            }
            continue;
        }
        return TRUE;
    }
    const fakefocus::SoftInputState* st = SoftState();
    if (st && (st->flags & fakefocus::kSoftFlagPostKeyEvents)) return FALSE;
    if (!MessageFilterIncludesWmInput(min, max)) return FALSE;
    if (!FillFakeInputMsg(lpMsg, hWnd)) return FALSE;
    return TRUE;
}

BOOL WINAPI Hook_PeekMessageW(LPMSG lpMsg, HWND hWnd, UINT min, UINT max, UINT remove) {
    return HookPeekMessage(lpMsg, hWnd, min, max, remove, true);
}
BOOL WINAPI Hook_PeekMessageA(LPMSG lpMsg, HWND hWnd, UINT min, UINT max, UINT remove) {
    return HookPeekMessage(lpMsg, hWnd, min, max, remove, false);
}

BOOL WINAPI Hook_GetMessageW(LPMSG lpMsg, HWND hWnd, UINT min, UINT max) {
    if (HookPeekMessage(lpMsg, hWnd, min, max, PM_REMOVE, true)) return TRUE;
    return CallOriginalGetMessage(lpMsg, hWnd, min, max, true);
}
BOOL WINAPI Hook_GetMessageA(LPMSG lpMsg, HWND hWnd, UINT min, UINT max) {
    if (HookPeekMessage(lpMsg, hWnd, min, max, PM_REMOVE, false)) return TRUE;
    return CallOriginalGetMessage(lpMsg, hWnd, min, max, false);
}

UINT WINAPI Hook_GetRawInputData(HRAWINPUT hRawInput, UINT uiCommand, LPVOID pData,
    PUINT pcbSize, UINT cbSizeHeader) {
    if (hRawInput != FakeRawHandle()) {
        const UINT result = CallOriginalRawData(hRawInput, uiCommand, pData, pcbSize, cbSizeHeader);
        if (SoftCursorBlocksPhysicalMouse()
            && uiCommand == RID_INPUT
            && pData
            && result != static_cast<UINT>(-1)
            && result >= sizeof(RAWINPUTHEADER)) {
            auto* ri = static_cast<RAWINPUT*>(pData);
            if (ri->header.dwType == RIM_TYPEMOUSE) {
                ri->data.mouse.lLastX = 0;
                ri->data.mouse.lLastY = 0;
                // ⚠⚠ 这里**只清光标与按键**，滚轮必须放行（2026-09-30）：
                //   这一段是"用户真鼠标的移动不要带着游戏视角乱转"的隔离。
                //   但滚轮是**离散的用户意图**，不是持续状态 —— 连它一起清零，
                //   用户自己手动滚一下都会被吞掉（"滚轮完全没反应"的第二种成因）。
                //   下面只在**确实是滚轮事件**时才保留，其余按键位仍照旧清掉。
                if (ri->data.mouse.usButtonFlags
                    & (RI_MOUSE_WHEEL | RI_MOUSE_HWHEEL)) {
                    ri->data.mouse.usButtonFlags &=
                        static_cast<USHORT>(RI_MOUSE_WHEEL | RI_MOUSE_HWHEEL);
                } else {
                    ri->data.mouse.usButtonFlags = 0;
                    ri->data.mouse.usButtonData = 0;
                }
            }
        }
        return result;
    }
    if (!pcbSize) return static_cast<UINT>(-1);
    FillPendingRawInput();
    if (!g_rawValid) return static_cast<UINT>(-1);

    if (uiCommand == RID_HEADER) {
        const UINT need = sizeof(RAWINPUTHEADER);
        if (!pData) {
            *pcbSize = need;
            return 0;
        }
        if (*pcbSize < need) {
            *pcbSize = need;
            return static_cast<UINT>(-1);
        }
        std::memcpy(pData, &g_rawPending.header, need);
        *pcbSize = need;
        return need;
    }
    if (uiCommand == RID_INPUT) {
        const UINT need = static_cast<UINT>(g_rawPending.header.dwSize);
        if (!pData) {
            *pcbSize = need;
            return 0;
        }
        if (*pcbSize < need) {
            *pcbSize = need;
            return static_cast<UINT>(-1);
        }
        std::memcpy(pData, &g_rawPending, need);
        *pcbSize = need;
        ConsumePendingRawInput();
        return need;
    }
    return static_cast<UINT>(-1);
}

UINT WINAPI Hook_GetRawInputBuffer(PRAWINPUT pData, PUINT pcbSize, UINT cbSizeHeader) {
    const UINT inSize = pcbSize ? *pcbSize : 0;
    const bool swallow = SoftCursorBlocksPhysicalMouse();
    if (swallow) {
        BYTE sink[2048];
        UINT sinkSize = sizeof(sink);
        CallOriginalRawBuffer(reinterpret_cast<PRAWINPUT>(sink), &sinkSize, cbSizeHeader);
        if (pcbSize) *pcbSize = inSize;
    } else {
        const UINT orig = CallOriginalRawBuffer(pData, pcbSize, cbSizeHeader);
        if (orig != 0 && orig != static_cast<UINT>(-1)) return orig;
    }
    if (!FillPendingRawInput()) return 0;
    const UINT need = static_cast<UINT>(g_rawPending.header.dwSize);
    if (!pcbSize) return static_cast<UINT>(-1);
    if (!pData) {
        *pcbSize = need;
        return static_cast<UINT>(-1);
    }
    if (inSize < need) {
        *pcbSize = need;
        return static_cast<UINT>(-1);
    }
    std::memcpy(pData, &g_rawPending, need);
    *pcbSize = need;
    ConsumePendingRawInput();
    return 1;
}

void RemoveRawInputHooks() {
    fakefocus::RemoveInlineHook(g_hookGetMsgA);
    fakefocus::RemoveInlineHook(g_hookGetMsgW);
    fakefocus::RemoveInlineHook(g_hookPeekA);
    fakefocus::RemoveInlineHook(g_hookPeekW);
    fakefocus::RemoveInlineHook(g_hookRawBuffer);
    fakefocus::RemoveInlineHook(g_hookRawData);
}

void MaybePostFakeWmInput() {
    if (g_rawValid) return;
    if (!FillPendingRawInput()) return;
    HWND top = g_targetTop.load(std::memory_order_relaxed);
    if (!top || !IsWindow(top)) return;
    PostMessageW(top, WM_INPUT, RIM_INPUT, reinterpret_cast<LPARAM>(FakeRawHandle()));
}

void InstallRawInputHooks(HMODULE user32, bool lite) {
    if (!user32) return;
    void* pData = reinterpret_cast<void*>(GetProcAddress(user32, "GetRawInputData"));
    void* pBuf = reinterpret_cast<void*>(GetProcAddress(user32, "GetRawInputBuffer"));
    if (pData) {
        fakefocus::InstallInlineHook(g_hookRawData, pData, reinterpret_cast<void*>(&Hook_GetRawInputData));
    }
    if (pBuf) {
        fakefocus::InstallInlineHook(g_hookRawBuffer, pBuf, reinterpret_cast<void*>(&Hook_GetRawInputBuffer));
    }
    if (lite) return;
    void* pPeekW = reinterpret_cast<void*>(GetProcAddress(user32, "PeekMessageW"));
    void* pPeekA = reinterpret_cast<void*>(GetProcAddress(user32, "PeekMessageA"));
    void* pGetW = reinterpret_cast<void*>(GetProcAddress(user32, "GetMessageW"));
    void* pGetA = reinterpret_cast<void*>(GetProcAddress(user32, "GetMessageA"));
    if (pPeekW) {
        fakefocus::InstallInlineHook(g_hookPeekW, pPeekW, reinterpret_cast<void*>(&Hook_PeekMessageW));
    }
    if (pPeekA) {
        fakefocus::InstallInlineHook(g_hookPeekA, pPeekA, reinterpret_cast<void*>(&Hook_PeekMessageA));
    }
    if (pGetW) {
        fakefocus::InstallInlineHook(g_hookGetMsgW, pGetW, reinterpret_cast<void*>(&Hook_GetMessageW));
    }
    if (pGetA) {
        fakefocus::InstallInlineHook(g_hookGetMsgA, pGetA, reinterpret_cast<void*>(&Hook_GetMessageA));
    }
}

void PrimeFakeFocusWindow(HWND top, HWND preserveFg) {
    if (!top || !IsWindow(top)) return;
    LockSetForegroundWindow(LSFW_LOCK);
    DWORD_PTR dummy = 0;
    SendMessageTimeoutW(top, WM_NCACTIVATE, TRUE, 0,
        SMTO_ABORTIFHUNG | SMTO_NORMAL, 200, &dummy);
    SendMessageTimeoutW(top, WM_ACTIVATE, WA_ACTIVE, 0,
        SMTO_ABORTIFHUNG | SMTO_NORMAL, 200, &dummy);
    SendMessageTimeoutW(top, WM_SETFOCUS, 0, 0,
        SMTO_ABORTIFHUNG | SMTO_NORMAL, 200, &dummy);
    LockSetForegroundWindow(LSFW_UNLOCK);
    if (preserveFg && IsWindow(preserveFg) && preserveFg != top) {
        SetForegroundWindow(preserveFg);
    }
}
