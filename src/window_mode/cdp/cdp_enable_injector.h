#pragma once

#include <windows.h>

#include <string>

namespace windowmode {

/// Returns true if CDP HTTP list becomes reachable on preferredPort (or nearby / DevToolsActivePort).
bool EnableCdpViaProcessInject(HWND edgeTop, int preferredPort, std::wstring& err);

}  // namespace windowmode
