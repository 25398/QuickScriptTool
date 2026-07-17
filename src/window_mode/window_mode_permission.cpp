#include "window_mode_permission.h"

namespace windowmode {

namespace {

bool QueryTokenElevation(HANDLE token, bool& elevated) {
    elevated = false;
    if (!token) return false;

    TOKEN_ELEVATION elev{};
    DWORD len = 0;
    if (!GetTokenInformation(token, TokenElevation, &elev, sizeof(elev), &len)) {
        return false;
    }
    elevated = elev.TokenIsElevated != 0;
    return true;
}

}  // namespace

bool IsCurrentProcessElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    bool elevated = false;
    const bool ok = QueryTokenElevation(token, elevated);
    CloseHandle(token);
    return ok && elevated;
}

bool IsProcessElevated(DWORD pid) {
    if (pid == 0) return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;

    HANDLE token = nullptr;
    bool elevated = false;
    bool ok = false;
    if (OpenProcessToken(process, TOKEN_QUERY, &token)) {
        ok = QueryTokenElevation(token, elevated);
        CloseHandle(token);
    }
    CloseHandle(process);
    return ok && elevated;
}

bool CheckPermissionMatch(DWORD targetPid) {
    if (targetPid == 0) return true;

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, targetPid);
    if (!process) return false;

    HANDLE token = nullptr;
    bool targetElevated = false;
    bool known = false;
    if (OpenProcessToken(process, TOKEN_QUERY, &token)) {
        known = QueryTokenElevation(token, targetElevated);
        CloseHandle(token);
    }
    CloseHandle(process);
    if (!known) return false;

    return IsCurrentProcessElevated() == targetElevated;
}

}  // namespace windowmode
