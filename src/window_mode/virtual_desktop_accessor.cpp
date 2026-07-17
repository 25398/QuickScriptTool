#include "virtual_desktop_accessor.h"

#include "com_apartment.h"
#include "window_mode_requirements.h"

#include <cstdlib>
#include <objbase.h>
#include <vector>
#include <winternl.h>

namespace windowmode {

namespace {

struct VdaDllCandidate {
    const wchar_t* fileName;
    DWORD minBuild;
    DWORD maxBuild;
};

constexpr VdaDllCandidate kDllCandidates[] = {
    {L"VirtualDesktopAccessor11.dll", 22000, UINT32_MAX},
    {L"VirtualDesktopAccessor10.dll", 10240, 21999},
    {L"VirtualDesktopAccessor.dll", 22000, UINT32_MAX},
};


std::string WideToUtf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string utf8(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, utf8.data(), len, nullptr, nullptr);
    if (!utf8.empty() && utf8.back() == '\0') utf8.pop_back();
    return utf8;
}

std::wstring TrimCopy(std::wstring value) {
    while (!value.empty() && (value.front() == L' ' || value.front() == L'\t')) value.erase(value.begin());
    while (!value.empty() && (value.back() == L' ' || value.back() == L'\t')) value.pop_back();
    return value;
}

bool NamesEqual(const std::wstring& a, const std::wstring& b) {
    return TrimCopy(a) == TrimCopy(b);
}

bool GuidFromString(const wchar_t* text, GUID& outId) {
    if (!text || !text[0]) return false;
    return CLSIDFromString(const_cast<LPWSTR>(text), &outId) == NOERROR;
}

int FindDesktopIndexByRegistryName(const std::wstring& expectedName, GUID& outId) {
    HKEY desktopsKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\VirtualDesktops\\Desktops",
            0, KEY_READ, &desktopsKey) != ERROR_SUCCESS) {
        return -1;
    }

    int foundIndex = -1;
    DWORD enumIndex = 0;
    wchar_t subName[128]{};
    DWORD subNameLen = static_cast<DWORD>(std::size(subName));
    while (RegEnumKeyExW(desktopsKey, enumIndex++, subName, &subNameLen,
            nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
        HKEY desktopKey = nullptr;
        if (RegOpenKeyExW(desktopsKey, subName, 0, KEY_READ, &desktopKey) == ERROR_SUCCESS) {
            wchar_t nameBuf[256]{};
            DWORD nameLen = sizeof(nameBuf);
            DWORD type = 0;
            if (RegQueryValueExW(desktopKey, L"Name", nullptr, &type,
                    reinterpret_cast<LPBYTE>(nameBuf), &nameLen) == ERROR_SUCCESS
                && (type == REG_SZ || type == REG_EXPAND_SZ)
                && NamesEqual(nameBuf, expectedName)) {
                GUID id{};
                if (GuidFromString(subName, id)) outId = id;
                RegCloseKey(desktopKey);
                foundIndex = 0;  // marker: found in registry
                break;
            }
            RegCloseKey(desktopKey);
        }
        subNameLen = static_cast<DWORD>(std::size(subName));
    }
    RegCloseKey(desktopsKey);
    return foundIndex;
}

std::wstring Utf8ToWide(const char* utf8, size_t byteLen) {
    if (!utf8 || byteLen == 0) return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, utf8, static_cast<int>(byteLen), nullptr, 0);
    if (len <= 0) return {};
    std::wstring wide(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, static_cast<int>(byteLen), wide.data(), len);
    return wide;
}

DWORD OsBuildNumber() {
    RTL_OSVERSIONINFOW vi{};
    vi.dwOSVersionInfoSize = sizeof(vi);
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    const auto rtlGetVersion = reinterpret_cast<RtlGetVersionFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    if (!rtlGetVersion || rtlGetVersion(&vi) != 0) return 0;
    return vi.dwBuildNumber;
}

bool GuidIsEmpty(const GUID& id) {
    GUID empty{};
    return IsEqualGUID(id, empty);
}

}  // namespace

VirtualDesktopAccessor& VirtualDesktopAccessor::Instance() {
    static VirtualDesktopAccessor inst;
    return inst;
}

std::wstring VirtualDesktopAccessor::BuildCandidatePath(const wchar_t* fileName) const {
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring dir = exePath;
    const auto slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) dir.resize(slash + 1);
    return dir + fileName;
}

bool VirtualDesktopAccessor::TryLoadDll(const std::wstring& path, std::wstring& err) {
    Unload();

    HMODULE mod = LoadLibraryW(path.c_str());
    if (!mod) {
        err = L"无法加载 " + path + L" (error " + std::to_wstring(GetLastError()) + L")";
        return false;
    }

    module_ = mod;
    loadedDllName_ = path;
    ResolveFunctions();

    if (!getDesktopCount_) {
        err = L"DLL 缺少 GetDesktopCount: " + path;
        Unload();
        return false;
    }

    const int count = getDesktopCount_();
    if (count < 0) {
        err = L"DLL 初始化失败 (GetDesktopCount=-1): " + path;
        Unload();
        return false;
    }

    err.clear();
    return true;
}

void VirtualDesktopAccessor::Unload() {
    if (!module_) {
        getDesktopCount_ = nullptr;
        getDesktopName_ = nullptr;
        setDesktopName_ = nullptr;
        createDesktop_ = nullptr;
        moveWindowToDesktopNumber_ = nullptr;
        getWindowDesktopNumber_ = nullptr;
        isWindowOnDesktopNumber_ = nullptr;
        isWindowOnCurrentVirtualDesktop_ = nullptr;
        getDesktopIdByNumber_ = nullptr;
        getWindowDesktopId_ = nullptr;
        getCurrentDesktopNumber_ = nullptr;
        goToDesktopNumber_ = nullptr;
        isPinnedWindow_ = nullptr;
        pinWindow_ = nullptr;
        unPinWindow_ = nullptr;
        loadedDllName_.clear();
        return;
    }

    HMODULE mod = module_;
    module_ = nullptr;
    loadedDllName_.clear();
    getDesktopCount_ = nullptr;
    getDesktopName_ = nullptr;
    setDesktopName_ = nullptr;
    createDesktop_ = nullptr;
    moveWindowToDesktopNumber_ = nullptr;
    getWindowDesktopNumber_ = nullptr;
    isWindowOnDesktopNumber_ = nullptr;
    isWindowOnCurrentVirtualDesktop_ = nullptr;
    getDesktopIdByNumber_ = nullptr;
    getWindowDesktopId_ = nullptr;
    getCurrentDesktopNumber_ = nullptr;
    goToDesktopNumber_ = nullptr;
    isPinnedWindow_ = nullptr;
    pinWindow_ = nullptr;
    unPinWindow_ = nullptr;
    FreeLibrary(mod);
}

void VirtualDesktopAccessor::ResolveFunctions() {
    if (!module_) return;

    auto load = [this](const char* name) -> FARPROC {
        return GetProcAddress(module_, name);
    };

    getDesktopCount_ = reinterpret_cast<FnGetDesktopCount>(load("GetDesktopCount"));
    getDesktopName_ = reinterpret_cast<FnGetDesktopName>(load("GetDesktopName"));
    setDesktopName_ = reinterpret_cast<FnSetDesktopName>(load("SetDesktopName"));
    createDesktop_ = reinterpret_cast<FnCreateDesktop>(load("CreateDesktop"));
    moveWindowToDesktopNumber_ = reinterpret_cast<FnMoveWindowToDesktopNumber>(
        load("MoveWindowToDesktopNumber"));
    getWindowDesktopNumber_ = reinterpret_cast<FnGetWindowDesktopNumber>(
        load("GetWindowDesktopNumber"));
    isWindowOnDesktopNumber_ = reinterpret_cast<FnIsWindowOnDesktopNumber>(
        load("IsWindowOnDesktopNumber"));
    isWindowOnCurrentVirtualDesktop_ = reinterpret_cast<FnIsWindowOnCurrentVirtualDesktop>(
        load("IsWindowOnCurrentVirtualDesktop"));
    getDesktopIdByNumber_ = reinterpret_cast<FnGetDesktopIdByNumber>(load("GetDesktopIdByNumber"));
    getWindowDesktopId_ = reinterpret_cast<FnGetWindowDesktopId>(load("GetWindowDesktopId"));
    getCurrentDesktopNumber_ = reinterpret_cast<FnGetCurrentDesktopNumber>(
        load("GetCurrentDesktopNumber"));
    goToDesktopNumber_ = reinterpret_cast<FnGoToDesktopNumber>(load("GoToDesktopNumber"));
    isPinnedWindow_ = reinterpret_cast<FnIsPinnedWindow>(load("IsPinnedWindow"));
    pinWindow_ = reinterpret_cast<FnPinWindow>(load("PinWindow"));
    unPinWindow_ = reinterpret_cast<FnUnPinWindow>(load("UnPinWindow"));
}

bool VirtualDesktopAccessor::EnsureLoaded(std::wstring& err) {
    // VDA caches COM desktop managers; apartment must stay alive for the thread.
    EnsureThreadComApartment();

    // FreeLibrary VDA before CRT teardown — otherwise PROCESS_DETACH + live COM can hang.
    static const bool kUnloadAtExit = []() {
        std::atexit([]() { VirtualDesktopAccessor::Instance().Unload(); });
        return true;
    }();
    (void)kUnloadAtExit;

    if (module_ && getDesktopCount_ && getDesktopCount_() >= 0) return true;

    const DWORD build = OsBuildNumber();
    std::wstring lastErr;

    for (const VdaDllCandidate& cand : kDllCandidates) {
        if (build < cand.minBuild || build > cand.maxBuild) continue;
        const std::wstring path = BuildCandidatePath(cand.fileName);
        if (TryLoadDll(path, lastErr)) return true;
    }

    for (const VdaDllCandidate& cand : kDllCandidates) {
        const std::wstring path = BuildCandidatePath(cand.fileName);
        if (TryLoadDll(path, lastErr)) return true;
    }

    err = lastErr.empty()
        ? L"未找到可用的 VirtualDesktopAccessor DLL，请将 DLL 放在程序目录"
        : lastErr;
    return false;
}

int VirtualDesktopAccessor::GetDesktopCount() const {
    return getDesktopCount_ ? getDesktopCount_() : -1;
}

std::wstring VirtualDesktopAccessor::GetDesktopName(int desktopNumber) const {
    if (!getDesktopName_ || desktopNumber < 0) return {};
    std::vector<char> buf(1024, '\0');
    if (getDesktopName_(desktopNumber, buf.data(), buf.size()) < 0) return {};
    const size_t len = strnlen(buf.data(), buf.size());
    return Utf8ToWide(buf.data(), len);
}

bool VirtualDesktopAccessor::SetDesktopName(int desktopNumber, const std::wstring& name) const {
    if (!setDesktopName_ || desktopNumber < 0) return false;
    const std::string utf8 = WideToUtf8(name);
    return setDesktopName_(desktopNumber, utf8.c_str()) >= 0;
}

int VirtualDesktopAccessor::CreateDesktop() const {
    return createDesktop_ ? createDesktop_() : -1;
}

int VirtualDesktopAccessor::GetCurrentDesktopNumber() const {
    return getCurrentDesktopNumber_ ? getCurrentDesktopNumber_() : -1;
}

bool VirtualDesktopAccessor::GoToDesktopNumber(int desktopNumber) const {
    if (!goToDesktopNumber_ || desktopNumber < 0) return false;
    return goToDesktopNumber_(desktopNumber) >= 0;
}

int VirtualDesktopAccessor::CreateDesktopPreservingView() const {
    // CreateDesktop 常会切到新桌面：立刻拉回，避免留在「鼠标宏」。
    const int viewDesktop = GetCurrentDesktopNumber();
    const int created = CreateDesktop();
    if (viewDesktop >= 0) {
        for (int i = 0; i < 12; ++i) {
            const int now = GetCurrentDesktopNumber();
            if (now == viewDesktop) break;
            GoToDesktopNumber(viewDesktop);
            Sleep(8);
        }
    }
    return created;
}

bool VirtualDesktopAccessor::MoveWindowToDesktopNumber(HWND hwnd, int desktopNumber) const {
    if (!moveWindowToDesktopNumber_ || desktopNumber < 0 || !hwnd) return false;
    return moveWindowToDesktopNumber_(hwnd, desktopNumber) >= 0;
}

bool VirtualDesktopAccessor::MoveWindowToDesktopNumberPreservingView(HWND hwnd,
    int desktopNumber) const {
    // 搬窗前调用方应已最小化/隐藏；若系统仍切走视图，立刻还原用户桌面。
    const int viewBefore = GetCurrentDesktopNumber();
    const bool ok = MoveWindowToDesktopNumber(hwnd, desktopNumber);
    if (viewBefore >= 0) {
        for (int i = 0; i < 8; ++i) {
            const int now = GetCurrentDesktopNumber();
            if (now < 0 || now == viewBefore) break;
            GoToDesktopNumber(viewBefore);
            Sleep(8);
        }
    }
    return ok;
}

int VirtualDesktopAccessor::GetWindowDesktopNumber(HWND hwnd) const {
    return getWindowDesktopNumber_ ? getWindowDesktopNumber_(hwnd) : -1;
}

bool VirtualDesktopAccessor::IsWindowOnDesktopNumber(HWND hwnd, int desktopNumber) const {
    if (!isWindowOnDesktopNumber_ || desktopNumber < 0 || !hwnd) return false;
    return isWindowOnDesktopNumber_(hwnd, desktopNumber) > 0;
}

int VirtualDesktopAccessor::IsWindowOnCurrentVirtualDesktop(HWND hwnd) const {
    if (!isWindowOnCurrentVirtualDesktop_ || !hwnd) return -1;
    return isWindowOnCurrentVirtualDesktop_(hwnd);
}

bool VirtualDesktopAccessor::GetDesktopIdByNumber(int desktopNumber, GUID& outId) const {
    if (!getDesktopIdByNumber_ || desktopNumber < 0) return false;
    outId = getDesktopIdByNumber_(desktopNumber);
    return !GuidIsEmpty(outId);
}

bool VirtualDesktopAccessor::GetWindowDesktopId(HWND hwnd, GUID& outId) const {
    if (!getWindowDesktopId_ || !hwnd) return false;
    outId = getWindowDesktopId_(hwnd);
    return !GuidIsEmpty(outId);
}

bool VirtualDesktopAccessor::DesktopNameMatches(int desktopNumber,
    const std::wstring& expectedName) const {
    if (desktopNumber < 0 || expectedName.empty()) return false;
    const std::wstring apiName = GetDesktopName(desktopNumber);
    if (!apiName.empty() && NamesEqual(apiName, expectedName)) return true;

    GUID id{};
    if (!GetDesktopIdByNumber(desktopNumber, id)) return false;

    HKEY desktopsKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\VirtualDesktops\\Desktops",
            0, KEY_READ, &desktopsKey) != ERROR_SUCCESS) {
        return false;
    }

    wchar_t guidStr[64]{};
    if (StringFromGUID2(id, guidStr, static_cast<int>(std::size(guidStr))) <= 0) {
        RegCloseKey(desktopsKey);
        return false;
    }

    HKEY desktopKey = nullptr;
    bool matched = false;
    if (RegOpenKeyExW(desktopsKey, guidStr, 0, KEY_READ, &desktopKey) == ERROR_SUCCESS) {
        wchar_t nameBuf[256]{};
        DWORD nameLen = sizeof(nameBuf);
        DWORD type = 0;
        if (RegQueryValueExW(desktopKey, L"Name", nullptr, &type,
                reinterpret_cast<LPBYTE>(nameBuf), &nameLen) == ERROR_SUCCESS
            && (type == REG_SZ || type == REG_EXPAND_SZ)) {
            matched = NamesEqual(nameBuf, expectedName);
        }
        RegCloseKey(desktopKey);
    }
    RegCloseKey(desktopsKey);
    return matched;
}

bool VirtualDesktopAccessor::FindDesktopIndexByGuid(const GUID& desktopId, int& outIndex) const {
    if (GuidIsEmpty(desktopId)) return false;
    const int count = GetDesktopCount();
    for (int i = 0; i < count; ++i) {
        GUID id{};
        if (GetDesktopIdByNumber(i, id) && IsEqualGUID(id, desktopId)) {
            outIndex = i;
            return true;
        }
    }
    return false;
}

int VirtualDesktopAccessor::FindDesktopIndexByName(const std::wstring& expectedName) const {
    if (expectedName.empty()) return -1;

    const int count = GetDesktopCount();
    for (int i = 0; i < count; ++i) {
        const std::wstring apiName = GetDesktopName(i);
        if (!apiName.empty() && NamesEqual(apiName, expectedName)) return i;
    }

    for (int i = 0; i < count; ++i) {
        if (DesktopNameMatches(i, expectedName)) return i;
    }

    GUID registryId{};
    if (FindDesktopIndexByRegistryName(expectedName, registryId) >= 0) {
        int index = -1;
        if (FindDesktopIndexByGuid(registryId, index)) return index;
    }

    return -1;
}

int VirtualDesktopAccessor::IsPinnedWindow(HWND hwnd) const {
    if (!isPinnedWindow_ || !hwnd) return -1;
    return isPinnedWindow_(hwnd);
}

bool VirtualDesktopAccessor::PinWindow(HWND hwnd) const {
    if (!pinWindow_ || !hwnd) return false;
    return pinWindow_(hwnd) >= 0;
}

bool VirtualDesktopAccessor::UnPinWindow(HWND hwnd) const {
    if (!unPinWindow_ || !hwnd) return false;
    return unPinWindow_(hwnd) >= 0;
}

}  // namespace windowmode
