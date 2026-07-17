#pragma once

#include <windows.h>

#include <string>

namespace windowmode {

class WindowModePreview {
public:
    void Bind(HWND owner, HFONT font);
    void Show(int x, int y, int w, int h);
    void Hide();
    void Destroy();
    void Refresh(HWND targetHwnd);
    bool IsVisible() const;

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

private:

    HWND owner_ = nullptr;
    HWND hwnd_ = nullptr;
    HFONT font_ = nullptr;
    HBITMAP previewBmp_ = nullptr;
};

}  // namespace windowmode
