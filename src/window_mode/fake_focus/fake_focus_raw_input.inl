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
void* CallOrigPeek(void* raw) {
    auto* ctx = static_cast<PeekCallCtx*>(raw);
    ctx->ok = ctx->unicode
        ? PeekMessageW(ctx->lpMsg, ctx->hWnd, ctx->min, ctx->max, ctx->remove)
        : PeekMessageA(ctx->lpMsg, ctx->hWnd, ctx->min, ctx->max, ctx->remove);
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
void* CallOrigGetMsg(void* raw) {
    auto* ctx = static_cast<GetMsgCallCtx*>(raw);
    ctx->ok = ctx->unicode
        ? GetMessageW(ctx->lpMsg, ctx->hWnd, ctx->min, ctx->max)
        : GetMessageA(ctx->lpMsg, ctx->hWnd, ctx->min, ctx->max);
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
void* CallOrigRawData(void* raw) {
    auto* ctx = static_cast<RawDataCallCtx*>(raw);
    ctx->result = GetRawInputData(ctx->hRaw, ctx->cmd, ctx->pData, ctx->pcbSize, ctx->cbHeader);
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
void* CallOrigRawBuf(void* raw) {
    auto* ctx = static_cast<RawBufCallCtx*>(raw);
    ctx->result = GetRawInputBuffer(ctx->pData, ctx->pcbSize, ctx->cbHeader);
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
                ri->data.mouse.usButtonFlags = 0;
                ri->data.mouse.usButtonData = 0;
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
