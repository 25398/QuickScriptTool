#include "hidden_desktop.h"

namespace windowmode {

HiddenDesktop::~HiddenDesktop() {
    Close();
}

bool HiddenDesktop::OpenOrCreate() {
    return virtualDesktop_.OpenOrCreate();
}

void HiddenDesktop::Close() {
    virtualDesktop_.Close();
}

bool HiddenDesktop::IsValid() const {
    return virtualDesktop_.IsValid();
}

bool HiddenDesktop::LaunchProcess(const std::wstring& exe, const std::wstring& args,
    PROCESS_INFORMATION& outPi, const std::wstring& titleContains) {
    return virtualDesktop_.LaunchProcess(exe, args, outPi, titleContains);
}

}  // namespace windowmode
