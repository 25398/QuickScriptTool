// ── 连点器功能实现 ──────────────────────────────────────────
#include "main_window.h"

// --------------------------------------------------
    void MainWindow::ToggleClicker() {
        if (clicking_) { StopClicking(); } else { StartClicking(); }
    }

// --------------------------------------------------
    void MainWindow::StopClickerCleanup() { clicking_ = false; if (clickerThread_.joinable()) clickerThread_.join(); }
