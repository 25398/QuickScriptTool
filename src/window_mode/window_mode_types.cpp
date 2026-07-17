#include "window_mode_types.h"

namespace windowmode {

const wchar_t* HealthToDisplayText(WindowModeHealth health) {
    switch (health) {
    case WindowModeHealth::Ok: return L"就绪";
    case WindowModeHealth::Unknown: return L"未知";
    case WindowModeHealth::DesktopNotReady: return L"宏虚拟桌面未就绪";
    case WindowModeHealth::TargetNotFound: return L"未找到目标窗口";
    case WindowModeHealth::TargetMinimized: return L"目标已最小化";
    case WindowModeHealth::TargetNoRender: return L"目标无法渲染";
    case WindowModeHealth::CaptureFailed: return L"截图失败";
    case WindowModeHealth::PermissionMismatch: return L"权限不一致";
    default: return L"未知";
    }
}

const wchar_t* HealthToUserHint(WindowModeHealth health) {
    switch (health) {
    case WindowModeHealth::Ok: return L"窗口模式就绪";
    case WindowModeHealth::Unknown: return L"状态未知";
    case WindowModeHealth::DesktopNotReady: return L"无法创建或打开「鼠标宏」虚拟桌面";
    case WindowModeHealth::TargetNotFound: return L"请在「鼠标宏」虚拟桌面启动目标程序，或绑定当前窗口";
    case WindowModeHealth::TargetMinimized: return L"目标窗口已最小化，已尝试后台还原但仍失败，请手动还原";
    case WindowModeHealth::TargetNoRender: return L"该程序可能不支持后台截图（游戏/GPU 界面）";
    case WindowModeHealth::CaptureFailed: return L"截图失败，请稍后重试";
    case WindowModeHealth::PermissionMismatch: return L"请以相同权限运行本工具与目标程序";
    default: return L"";
    }
}

}  // namespace windowmode
