// ── 连点器功能实现 ──────────────────────────────────────────
#include "engine/engine_host_window.h"

// --------------------------------------------------
    void EngineHost::ToggleClicker() {
        if (clicking_) { StopClicking(); } else { StartClicking(); }
    }

// --------------------------------------------------
    void EngineHost::StopClickerCleanup() {
        clicking_ = false;
        if (!clickerThread_.joinable()) return;
        // 禁止无限 join：最多等 300ms，否则 detach（退出场景由 ExitProcess 回收）。
        HANDLE h = reinterpret_cast<HANDLE>(clickerThread_.native_handle());
        if (h && WaitForSingleObject(h, 300) == WAIT_OBJECT_0) {
            clickerThread_.join();
        } else {
            clickerThread_.detach();
        }
    }
