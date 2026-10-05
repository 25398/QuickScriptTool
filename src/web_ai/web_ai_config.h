#pragma once
// ──────────────────────────────────────────────────────────────────
// web_ai_config.h — 网页版 AI 的**开关与档案改写**
//
// 产品边界（`MEMORY.md` §14「产品不依赖别人的软件运行」）落实在这里：
//   · **不主动、不隐式**：绝不在用户没表态时把 AI 请求接到浏览器上；
//   · **用户显式选择即授权**：用户在「设置 → AI助手」的模型下拉里**选中**
//     这个档案，本身就是显式配置 —— 不需要再去建一个 json 文件。
//     （`EnsureProfileInSettings` 因此**始终**把档案注入 `savedModels`，
//      让它在下拉里**总能看到**；"选中"才真正启用。）
//   · **不抢占用户自己的 API**：档案一律**追加在末尾**，`ResolveAiModelName`
//     在没有 `ai.modelName` 时会取 `savedModels` 的**第一个** ——
//     插队会把用户自己的模型挤掉。
//
// 两条启用入口（任一成立即 `Enabled()`）：
//   ① 用户把 `ai.modelName` 选成了本档案（主路径，免 API Key）
//   ② `AppDir()\web_ai_config.json` 里写 `{"enabled": true}`（向后兼容 + 高级用户改参数）
// ──────────────────────────────────────────────────────────────────

#include <string>
#include <vector>

#include "app_settings.h"

namespace quickscript::webai {

struct UserConfig {
    bool enabled = false;
    /// 站点 id（doubao / deepseek / yuanbao），见 extension/edge/web_ai_providers.json
    std::string provider = "doubao";
    /// 注入 savedModels 用的模型名。
    ///
    /// ★ 2026-09-25 起**统一为 `web`**（用户要求）：一个选项，站点由运行时自动挑
    ///   （已打开的优先 → 已登录的其次 → 都没有就报错提示去登录）。
    ///   旧形态 `doubao-web`/`deepseek-web`/… 仍被认得，但会被**迁移**掉（见
    ///   `EnsureProfileInSettings`）。
    ///
    /// ⚠ 名字里**不再含** `doubao` ⇒ `ModelSupportsVision` 不会判它识图。
    ///   这不影响使用：网页通道的识图靠**传图**（见 web_ai_image.h），
    ///   而不是靠"模型名像不像多模态模型"。
    std::string modelName = "web";
    /// 单次「读回答」的空闲判据（内容稳定多久算生成结束）
    int idleTimeoutMs = 8000;
    /// 一轮回答的整体上限
    int maxTotalMs = 180000;
    /// 提示词预算（网页输入框能吃多少字**未实测**，超了只丢描述不丢工具名）
    int maxToolListChars = 12000;
    int maxTotalPromptChars = 24000;
    /// ★★ 读回答时是否**临时把我们那个标签页切到前台**。
    ///
    /// ⚠⚠ **默认 false**（2026-09-26 改）：用户明确抱怨"占用了界面前台，
    ///   使用此功能时用户无法正常使用浏览器和电脑"。前台化确实能让站点渲染
    ///   （后台标签页 `visibilityState === 'hidden'` ⇒ 站点不往 DOM 写流式回答），
    ///   但代价是抢走用户的屏幕 —— 不能默认这么做。
    ///
    ///   现在的默认路径是：扩展先注入 `visibilityState` 覆盖 + focus 模拟 + lifecycle active
    ///   （见扩展的 `webAiInstallVisibilityOverride` / `keepTargetAlive`），
    ///   **读不到回答时**用户再把这个开关打开。
    bool activateTabDuringRun = false;

    /// ★ 渐进披露：首轮只给**工具目录**（名字+参数名+一句话），
    ///   模型用 `loadTools` 索取完整定义（把每轮重发的 30~50 KB 压到 ~3 KB）。
    ///   设为 false 会退回"首轮就发全部完整定义" —— 只在模型不听话时才需要。
    bool toolCatalogMode = true;

    /// ★★ 窗口反代：要**暴露到模型下拉**里的客户端 id（如 `["doubao-app"]`）。
    ///
    /// 语义与网页端的 `web` 档案**刻意不同**：`web` 是"始终在列表里"，
    /// 而窗口客户端**用户点名才注入** —— 因为窗口通道要抢前台，且判据
    /// （输入框/回答容器的 UIA 属性）必须先按真实控件树校准（见
    /// `window_ai_providers.json` + `/qst/window-ai/probe`）。
    /// 没校准就塞进下拉 = 让用户选到一个必然失败的东西。
    std::vector<std::string> windowClients;
    /// 配置文件的原始路径（诊断用；文件不存在时为空）
    std::string configPath;
    /// 配置文件是否存在（不存在 ⇒ 功能关闭且不报错）
    bool fileExists = false;
};

/// 读配置（带缓存；`ReloadUserConfig()` 可强制重读）
const UserConfig& Config();
void ReloadUserConfig();

/// 当前进程是不是**自检/诊断**进程（`*SelfTest.exe` / `QstRecorderLogicTest.exe` / `*Probe.exe`）。
///
/// ⚠⚠ 这类进程**一律不读用户配置文件**（`Config()` 直接返回「未开启」）。理由：
///   自检 exe 的 `AppDir()` 就是 `build\Release\`，那里**正好会有**用户/开发机上的
///   `web_ai_config.json` ⇒ 用例结果会取决于「这台机器上有没有那个文件」：
///   在开发机上"恰好绿"、在干净 runner 上必红（本仓已有环境依赖用例的前科）。
///   要验解析就用纯函数 `ParseUserConfigJson`（不吃环境）。
bool CurrentProcessIsSelfTestOrProbe();

/// 功能是否开启（选中档案 **或** 配置文件开启，见文件头）
bool Enabled();

/// 注入/使用的模型名（宽字符，便于与 AiModelProfile 比较）
std::wstring ModelNameW();

/// 该模型名是否就是「网页 AI」档案
bool IsWebAiModelName(const std::wstring& modelName);

/// 按 **exe 文件名**判定是不是自检/诊断进程（纯函数，可逐格断言）
bool ExeNameIsSelfTestOrProbe(const std::wstring& exeName);

/// 当前进程是不是自检/诊断进程（`*SelfTest.exe` / `QstRecorderLogicTest.exe` / `*Probe.exe`）。
///
/// ⚠⚠ 这类进程**一律不读用户配置文件**（`Config()` 直接返回「未开启」）。理由：
///   自检 exe 的 `AppDir()` 就是 `build\Release\`，那里**正好会有**用户/开发机上的
///   `web_ai_config.json` ⇒ 用例结果会取决于「这台机器上有没有那个文件」：
///   在开发机上"恰好绿"、在干净 runner 上必红（本仓已有环境依赖用例的前科）。
///   要验解析就用纯函数 `ParseUserConfigJson`（不吃环境）。
bool CurrentProcessIsSelfTestOrProbe();

/// 桥的当前 token（桥没跑时返回空）
std::string BridgeToken();

/// 「本地端点」URL（桥端口取自运行中的桥；桥没跑时端口为 0 ⇒ 返回带占位端口的 URL）
std::string LocalEndpointUrl();

/// ★ 账号设置层面的注入（在 LoadAppSettings 之后调用一次即可，幂等）：
///   ① **始终**把档案补进 `savedModels`（用户在下拉里能看见它）
///   ② 若用户把 `ai.modelName` 选成了它 ⇒ 打开 AI 总闸、指向本机端点（`Enabled()` 转真）
///   ③ 用户没选它 ⇒ **什么都不改**（尤其不删档案、不动总闸）
void EnsureProfileInSettings(quickscript::AiApiSettings& ai);

/// 供自检用：用户是否已把默认模型选成本档案（决定 `Enabled()`）
bool UserSelectedWebAiModel();

/// ★ 三条 AgentCore 构造路径的统一改写点：把「网页 AI 档案」的
///   apiUrl 规范到**当前**桥端口、apiKey 换成**当前**桥 token。
///   返回是否发生了改写（供日志/自检断言）。
///   ⚠ 为什么必须在构造时改写而不是在设置里存好：桥每次启动都会换端口与 token
///     （`ext_bridge_server.cpp:520` 随机 token、`BindPort` 顺序占位）⇒
///     设置里存的那份**下一次启动就是错的**，而且是静默错（请求打到别的端口/401）。
bool ApplyProfileOverride(quickscript::AiModelProfile& profile);

/// 网页版 AI 的**唯一**模型名（`web`）。用户在设置页看到的就是它。
extern const wchar_t* const kWebAiModelName;

/// ★ **内置站点 id 列表**（顺序 = **自动挑选**的顺序，也是"哪些站点算内置"的定义）。
///
/// ⚠⚠ 加/删站点要**同时**改三处，漏一处的症状各不相同：
///   · 漏这里 ⇒ 设置页里**看不到**那个模型（用户以为没做）
///   · 漏 `ProviderFromModelName`（`web_ai_prompt.cpp`）⇒ 选了它会被判成「认不出站点」
///   · 漏 `extension/edge/web_ai_providers.json` ⇒ 请求报 `NO_PROVIDER`
const std::vector<std::wstring>& BuiltinWebAiProviderIds();

/// 站点 id → **旧形态**的模型名（`doubao` → `doubao-web`）。
/// ⚠ 仅用于**识别与迁移**旧档案；新档案一律叫 `kWebAiModelName`（`web`）。
std::wstring WebAiModelNameForProvider(const std::wstring& providerId);

/// 是不是旧的「每站点一个模型名」形态（`doubao-web` / `deepseek-web` / …）
bool IsLegacyProviderModelName(const std::wstring& modelName);

/// 这个名字是不是网页版 AI 档案（`web`，或旧形态 `<站点>-web`）
bool IsBuiltinWebAiModelName(const std::wstring& modelName);

/// ★ **纯函数**（不读盘、不问桥）：把网页 AI 档案注入设置。
///   实现放在 `web_ai_config_parse.cpp` —— 这样 `WebAiSelfTest` 能逐格断言
///   「档案始终在末尾 / 选中才开总闸 / 没选中什么都不动」这三条硬规则。
/// @param selectedOut 出参：用户是否已把默认模型选成了本档案（== `Enabled()` 的主判据）
void ApplyProfileToSettings(quickscript::AiApiSettings& ai,
                            const std::wstring& model,
                            const std::wstring& url,
                            const std::wstring& key,
                            bool& selectedOut);

/// ★ **纯函数**（不读盘、不问桥）：**迁移旧档案 + 注入唯一的 `web` 档案
///   + 注入配置里点名的窗口反代客户端档案**。
///
/// 这是 `EnsureProfileInSettings` 的全部逻辑（那边只负责把桥的端口/token 喂进来）。
/// ⚠ 为什么必须抽出来：自检**只能**调纯函数。若把这段留在 `EnsureProfileInSettings` 里，
///   用例就只能自己传 `kWebAiModelName` 去调 `ApplyProfileToSettings` ——
///   于是**"到底注入了哪个名字"这件事根本没人测**，
///   把实现改回 `doubao-web` 测试照样全绿（实测踩过：变异不变红 = 永真断言）。
///
/// @param windowClients 配置里 `windowClients` 点名的客户端 id（如 `doubao-app`）。
///        ⚠ **默认空**：窗口反代要先校准判据才可用（见 `window_ai_providers.json`），
///          没校准就塞进下拉只会让用户选到一个必然失败的东西。
///          ⇒ 用户显式写了才注入，与"档案始终在列表里"的网页端**刻意不同**。
void MigrateAndApplyProfiles(quickscript::AiApiSettings& ai,
                             const std::wstring& url,
                             const std::wstring& key,
                             bool& selectedOut,
                             const std::vector<std::string>& windowClients = {});

/// 供自检用：把一段 JSON 解析成 UserConfig（不读盘）。
/// ⚠ 实现放在 `web_ai_config_parse.cpp`（**纯逻辑**，不依赖 ExtBridgeServer），
///   这样 `WebAiSelfTest` 才能只链纯逻辑那份就把它逐格断言。
UserConfig ParseUserConfigJson(const std::string& jsonUtf8, bool fileExists);

}  // namespace quickscript::webai
