#pragma once

#include <windows.h>

namespace windowmode {

bool IsCurrentProcessElevated();
bool IsProcessElevated(DWORD pid);
/// UIPI：本进程完整性 >= 目标即可。管理员操作未提权目标返回 true。
bool CheckPermissionMatch(DWORD targetPid);

}  // namespace windowmode
