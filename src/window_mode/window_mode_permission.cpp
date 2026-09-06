#include "window_mode_permission.h"

#include <vector>

namespace windowmode {

namespace {

bool QueryTokenIntegrityRid(HANDLE token, DWORD& rid) {
    rid = SECURITY_MANDATORY_MEDIUM_RID;
    if (!token) return false;

    DWORD len = 0;
    GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &len);
    if (len == 0) return false;
    std::vector<BYTE> buf(len);
    if (!GetTokenInformation(token, TokenIntegrityLevel, buf.data(), len, &len)) {
        return false;
    }
    auto* til = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buf.data());
    if (!til || !til->Label.Sid || !IsValidSid(til->Label.Sid)) return false;
    const PUCHAR count = GetSidSubAuthorityCount(til->Label.Sid);
    if (!count || *count == 0) return false;
    rid = *GetSidSubAuthority(til->Label.Sid, static_cast<DWORD>(*count - 1));
    return true;
}

bool QueryProcessIntegrityRid(HANDLE process, DWORD& rid) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token)) return false;
    const bool ok = QueryTokenIntegrityRid(token, rid);
    CloseHandle(token);
    return ok;
}

DWORD CurrentProcessIntegrityRid() {
    HANDLE token = nullptr;
    DWORD rid = SECURITY_MANDATORY_MEDIUM_RID;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return rid;
    QueryTokenIntegrityRid(token, rid);
    CloseHandle(token);
    return rid;
}

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
    // UIPI：只能向相同或更低完整性发 Win32 消息/输入。
    // 本工具以管理员运行、目标是普通程序/浏览器 → 允许（旧逻辑要求「两边都管理员」会误拦）。
    // 本工具未提权、目标已提权 → 拒绝。
    if (targetPid == 0) return true;

    const DWORD selfRid = CurrentProcessIntegrityRid();
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, targetPid);
    if (!process) {
        // 查不到目标：可能是 UIPI，也可能是受保护进程。
        // 已高完整性时不是「请以相同权限」这类 UAC 问题，放行给后续路径报更具体的错。
        return selfRid >= SECURITY_MANDATORY_HIGH_RID;
    }

    DWORD targetRid = SECURITY_MANDATORY_MEDIUM_RID;
    const bool known = QueryProcessIntegrityRid(process, targetRid);
    CloseHandle(process);
    if (!known) {
        return selfRid >= SECURITY_MANDATORY_HIGH_RID;
    }
    return selfRid >= targetRid;
}

}  // namespace windowmode
