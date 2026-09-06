#pragma once
// Shared pick result — usable without WindowPickDialog (product OFF path).

#include <string>

namespace windowmode {

struct WindowPickResult {
    bool accepted = false;
    std::wstring windowTitle;
    std::wstring windowClassName;
    std::wstring childWindowClassName;
    std::wstring processPath;
    std::wstring documentPath;
    int pickX = 0;
    int pickY = 0;
};

}  // namespace windowmode
