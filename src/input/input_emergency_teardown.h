#pragma once
// 进程被杀/AV 挂起/LL 钩子卡死时：尽快卸钩并收起 HID 会话，避免真实鼠标整桌面假死。

#include <windows.h>

namespace input_emergency {

using ExtraTeardownFn = void (*)();

/// 注册额外卸钩（热键 LL / 录制 LL 等）。可重复调用；同函数只登记一次。
void RegisterExtraTeardown(ExtraTeardownFn fn);

/// 安装 atexit / 未处理异常收尾，并启动 LL 卡死看门狗线程。
void InstallProcessHandlers();

/// 幂等：先卸全部已知 LL 钩子，再 ForceTeardown 注入会话。
void TeardownNow();

/// 仅卸钩（看门狗优先路径，尽快恢复真实鼠标）。
void TeardownHooksOnly();

/// 包在 WH_*_LL 回调最外层：若回调长时间不返回，看门狗会卸钩。
class LlHookGuard {
public:
    LlHookGuard();
    ~LlHookGuard();
    LlHookGuard(const LlHookGuard&) = delete;
    LlHookGuard& operator=(const LlHookGuard&) = delete;
};

}  // namespace input_emergency
