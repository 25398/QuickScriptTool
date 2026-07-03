#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

/// 准星拖拽模式
enum class CrosshairDragMode {
    Coordinates,  ///< 获取屏幕坐标
    ProgramPath,  ///< 获取窗口进程路径
};

/// 单个准星按钮绑定
struct CrosshairDragBinding {
    CrosshairDragMode mode = CrosshairDragMode::Coordinates;
    HWND targetEdit = nullptr;
};

/// 可复用的准星拖拽控制器（坐标拾取 / 程序路径查找）
class CrosshairDragController {
public:
    void SetOwner(HWND owner) { owner_ = owner; }
    void SetDragCursor(HCURSOR cursor) { dragCursor_ = cursor; }

    void ClearButtons();
    void RegisterButton(HWND button, CrosshairDragBinding binding);

    bool IsCrosshairButton(HWND hwnd) const;
    bool TryGetBinding(HWND button, CrosshairDragBinding& out) const;
    HWND HitButton(int x, int y, const std::function<RECT(HWND)>& clientRect) const;

    bool IsActive() const { return active_; }
    void Begin(const CrosshairDragBinding& binding);
    void End();

    using CoordinateHandler = std::function<void(int x, int y)>;
    using ProgramPathHandler = std::function<void(const std::wstring& path)>;
    bool HandleMessage(UINT msg, WPARAM wp, LPARAM lp,
        CoordinateHandler onCoordinate, ProgramPathHandler onProgramPath);

    void DrawButton(DRAWITEMSTRUCT* dis, HFONT font, HWND hoveredButton) const;
    void InvalidateButtons() const;

private:
    HWND owner_ = nullptr;
    HCURSOR dragCursor_ = nullptr;
    LONG_PTR savedCursor_ = 0;
    RECT savedWindowRect_{};
    bool active_ = false;
    CrosshairDragMode mode_ = CrosshairDragMode::Coordinates;
    HWND targetEdit_ = nullptr;
    std::vector<std::pair<HWND, CrosshairDragBinding>> buttons_;
};
