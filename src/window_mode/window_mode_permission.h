#pragma once

#include <windows.h>

namespace windowmode {

bool IsCurrentProcessElevated();
bool IsProcessElevated(DWORD pid);
bool CheckPermissionMatch(DWORD targetPid);

}  // namespace windowmode
