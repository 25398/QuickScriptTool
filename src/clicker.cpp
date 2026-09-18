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
        HANDLE h = reinterpret_cast<HANDLE>(clickerThread_.native_handle());
        if (h && WaitForSingleObject(h, 800) == WAIT_OBJECT_0) {
            clickerThread_.join();
        } else {
            ForegroundInputRouter::Instance().ForceTeardown();
            if (h && WaitForSingleObject(h, 400) == WAIT_OBJECT_0) {
                clickerThread_.join();
            } else {
                clickerThread_.detach();
            }
        }
    }
