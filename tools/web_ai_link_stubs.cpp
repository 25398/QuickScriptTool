// ──────────────────────────────────────────────────────────────────
// tools/web_ai_link_stubs.cpp — 逻辑档自检的 web_ai 链接桩
//
// 谁需要它：链 `script_action_builder_core` 的三个**逻辑档** SelfTest
//   （ScriptActionBuilderSelfTest / ScriptIoSelfTest / ScriptSerializationSelfTest）。
//   那个库里有 agent_ai_actions.cpp，它调 webai::EnsureProfileInSettings / Enabled /
//   ModelNameW / IsWebAiModelName；而这四个符号的真身在 `src/web_ai/web_ai_config.cpp`，
//   它 include `window_mode/ext_bridge/ext_bridge_server.h`（浏览器桥 + HTTP + 端口）
//   —— 逻辑档不该背这些依赖，所以在这里给四个符号落地实现。
//   （实测：不补这四个符号时三个目标全部 LNK2019 + LNK1120。）
//
// ★ 为什么桩是「恒 false / 空名」而不是转发真身：
//   真身 Enabled() 取决于**本机设置**（用户有没有把默认模型选成「网页 AI」），
//   自检结果不该随机器变 —— `web_ai_config.cpp` 的 `Config()` 自己也有
//   `CurrentProcessIsSelfTestOrProbe()` 护栏，同一个道理。
//   这三个套件里也**没有**任何用例依赖网页 AI 语义（grep webai/网页版/WebAi：无命中）。
// ⚠ 网页 AI 的真实能力（档案注入规则、站点能不能写进去/读回来）由
//   `WebAiSelfTest`（纯逻辑）与产品进程内的 `POST /qst/web-ai/probe` 覆盖，
//   **不要**把这个桩当成能力判据。
// ──────────────────────────────────────────────────────────────────
#include "web_ai/web_ai_config.h"

#include <string>

namespace quickscript::webai {

bool Enabled() {
    return false;
}

std::wstring ModelNameW() {
    return {};
}

bool IsWebAiModelName(const std::wstring& /*modelName*/) {
    return false;
}

void EnsureProfileInSettings(quickscript::AiApiSettings& /*ai*/) {
    // 逻辑档不注入「网页 AI」档案：注不注入取决于本机 web_ai_config.json 与用户设置，
    // 那属于真机链路（见文件头）。
}

}  // namespace quickscript::webai
