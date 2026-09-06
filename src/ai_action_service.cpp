#include "ai_action_service.h"

#include "action_utils.h"
#include "agent_ai_actions.h"
#include "agent_web.h"
#include "ai_action_lookahead.h"
#include "ai_action_router.h"
#include "ai_logic_convert.h"
#include "color_match.h"
#include "image_match.h"
#include "macro_execute_tools.h"
#include "script_action_builder.h"
#include "utils.h"

#include <opencv2/opencv.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <thread>
#include <vector>




namespace {
std::vector<AgentTool> BuildLookaheadDryToolsLocal() {
    auto tools = BuildAiActionExecuteTools(nullptr, {});
    for (auto& t : tools) {
        const std::wstring name = t.name;
        t.execute = [name](const std::wstring&) -> std::wstring {
            return L"[LOOKAHEAD_DRY] " + name + L"（预规划未执行）";
        };
    }
    return tools;
}

std::vector<ToolCallRecord> LookaheadDefaultFetchImpl(
    const AgentConfig& config,
    const std::wstring& userPrompt,
    const std::atomic_bool& stopFlag,
    AiHttpAbortSlot* httpAbort,
    const std::atomic_bool& cancelWorker) {
    std::vector<ToolCallRecord> out;
    if (stopFlag.load() || cancelWorker.load()) return out;
    if (config.apiUrl.empty() || config.model.empty()) return out;
    AgentConfig cfg = config;
    cfg.recvTimeoutMs = (std::min)(cfg.recvTimeoutMs, 25000);
    cfg.maxTokens = (std::min)(cfg.maxTokens > 0 ? cfg.maxTokens : 1024, 1024);
    auto core = std::make_unique<AgentCore>(
        cfg,
        L"你是 Windows 桌面宏预规划助手。根据备忘与刚执行的工具摘要，"
        L"假设界面已按预期变化，只规划下一步工具。必须 tool_calls。"
        L"定位用 locateAndClick 短描述。预规划供宿主对照观察后选用。",
        BuildLookaheadDryToolsLocal());
    ChatMessage msg;
    msg.role = L"user";
    msg.content = userPrompt;
    AgentSendCallbacks cb;
    cb.cancelFlag = &stopFlag;
    cb.httpAbort = httpAbort;
    cb.preferNonStream = true;
    cb.toolChoice = L"required";
    cb.stopToolLoopAfterTools = []() { return true; };
    (void)core->SendMessage(msg, cb);
    if (stopFlag.load() || cancelWorker.load()) return out;
    const auto& hist = core->GetHistory();
    for (auto it = hist.rbegin(); it != hist.rend(); ++it) {
        if (it->role == L"assistant" && !it->tool_calls.empty()) {
            out = it->tool_calls;
            break;
        }
    }
    return out;
}

struct LookaheadFetchRegistrar {
    LookaheadFetchRegistrar() {
        RegisterAiLookaheadDefaultFetch(LookaheadDefaultFetchImpl);
    }
};
static LookaheadFetchRegistrar g_lookaheadFetchRegistrar;
}  // namespace

namespace {



std::string Base64Encode(const std::vector<uint8_t>& data) {

    static const char kTable[] =

        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string out;

    out.reserve(((data.size() + 2) / 3) * 4);

    for (size_t i = 0; i < data.size(); i += 3) {

        const uint32_t n = (static_cast<uint32_t>(data[i]) << 16)

            | ((i + 1 < data.size()) ? static_cast<uint32_t>(data[i + 1]) << 8 : 0)

            | ((i + 2 < data.size()) ? static_cast<uint32_t>(data[i + 2]) : 0);

        out.push_back(kTable[(n >> 18) & 63]);

        out.push_back(kTable[(n >> 12) & 63]);

        out.push_back(i + 1 < data.size() ? kTable[(n >> 6) & 63] : '=');

        out.push_back(i + 2 < data.size() ? kTable[n & 63] : '=');

    }

    return out;

}

std::vector<uint8_t> Base64Decode(const std::string& data) {
    auto rev = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::vector<uint8_t> out;
    out.reserve((data.size() / 4) * 3);
    int buf = 0;
    int bits = 0;
    for (char c : data) {
        if (c == '=') break;
        const int v = rev(c);
        if (v < 0) continue;
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((buf >> bits) & 0xFF));
        }
    }
    return out;
}

cv::Mat DecodeBase64Image(const std::string& base64) {
    if (base64.empty()) return {};
    const std::vector<uint8_t> bytes = Base64Decode(base64);
    if (bytes.empty()) return {};
    try {
        cv::Mat raw = cv::imdecode(bytes, cv::IMREAD_COLOR);
        if (raw.empty()) return {};
        return raw;
    } catch (...) {
        return {};
    }
}



cv::Mat MatFromBitmap(HBITMAP hbmp) {
    if (!hbmp) return {};
    BITMAP bm{};
    if (!GetObject(hbmp, sizeof(bm), &bm) || bm.bmWidth <= 0 || bm.bmHeight <= 0) return {};

    // 必须用 32bpp：24bpp GetDIBits 要求行宽 DWORD 对齐，OpenCV CV_8UC3 连续缓冲无 padding，
    // 宽非整除 4 时写越界 → Zoom 裁切阶段间歇性闪退。
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = bm.bmWidth;
    bmi.bmiHeader.biHeight = -bm.bmHeight;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    cv::Mat bgra(bm.bmHeight, bm.bmWidth, CV_8UC4);
    HDC hdc = GetDC(nullptr);
    if (!hdc) return {};
    const int lines = GetDIBits(hdc, hbmp, 0, static_cast<UINT>(bm.bmHeight),
        bgra.ptr(), &bmi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, hdc);
    if (lines <= 0) return {};

    cv::Mat bgr;
    cv::cvtColor(bgra, bgr, cv::COLOR_BGRA2BGR);
    return bgr;
}



std::wstring TruncateForLog(const std::wstring& text, size_t maxLen = 240) {

    if (text.size() <= maxLen) return text;

    return text.substr(0, maxLen) + L"...";

}



std::wstring ExtractJsonArrayFromText(const std::wstring& text) {
    // 跳过 [EXECUTED][OBSERVE] 等标记，找真正的 JSON 数组（首元素为 { 或 [）
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != L'[') continue;
        size_t j = i + 1;
        while (j < text.size() && (text[j] == L' ' || text[j] == L'\t' || text[j] == L'\r' || text[j] == L'\n'))
            ++j;
        if (j >= text.size()) break;
        if (text[j] != L'{' && text[j] != L'[') continue;
        int depth = 0;
        bool inStr = false;
        bool esc = false;
        for (size_t k = i; k < text.size(); ++k) {
            const wchar_t c = text[k];
            if (inStr) {
                if (esc) esc = false;
                else if (c == L'\\') esc = true;
                else if (c == L'"') inStr = false;
                continue;
            }
            if (c == L'"') { inStr = true; continue; }
            if (c == L'[') ++depth;
            else if (c == L']') {
                --depth;
                if (depth == 0) return text.substr(i, k - i + 1);
            }
        }
        break;
    }
    return L"";
}


void LogSubmittedActionsBrief(AiMacroLogFn logFn, const std::wstring& headline,
    const std::wstring& jsonText, size_t maxItems = 16) {
    if (!logFn) return;
    const std::wstring arr = ExtractJsonArrayFromText(jsonText);
    if (arr.empty()) {
        // 工具结果已改为短摘要（省 token），直接回显摘要行
        std::wstring brief = Trim(jsonText);
        // 去掉 [EXECUTED]… 标记行，留「已执行:…」
        const size_t nl = brief.find(L'\n');
        if (nl != std::wstring::npos) brief = Trim(brief.substr(nl + 1));
        if (brief.size() > 160) brief = brief.substr(0, 160) + L"…";
        logFn(headline + (brief.empty() ? L"（已执行）" : brief));
        return;
    }
    try {
        const nlohmann::json j = nlohmann::json::parse(ToUtf8(arr));
        if (!j.is_array() || j.empty()) {
            logFn(headline + L"（空）");
            return;
        }
        logFn(headline + std::to_wstring(j.size()) + L" 个：");
        size_t shown = 0;
        for (const auto& step : j) {
            if (!step.is_object()) continue;
            if (shown >= maxItems) {
                logFn(L"    · …共 " + std::to_wstring(j.size()) + L" 个，已省略");
                break;
            }
            nlohmann::json params = step;
            if (step.contains("action") && step["action"].is_string()) {
                params = step.value("params", nlohmann::json::object());
                if (!params.is_object()) params = nlohmann::json::object();
                std::string t = step["action"].get<std::string>();
                if (t == "mouseMove") t = "moveMouse";
                params["type"] = t;
            } else if (params.contains("type") && params["type"].is_string()
                && params["type"].get<std::string>() == "mouseMove") {
                params["type"] = "moveMouse";
            }
            if (params.contains("type") && params["type"].is_string()
                && params["type"].get<std::string>() == "stopMacro") {
                continue;
            }
            // 本地工具 / Alt+Tab 子动作不是宏 type，直接给中文标签
            if (params.contains("type") && params["type"].is_string()) {
                const std::string t = params["type"].get<std::string>();
                const wchar_t* label = nullptr;
                if (t == "openPreview") label = L"唤出 Alt+Tab 预览（按住 Alt）";
                else if (t == "move") label = L"预览内移动选中项";
                else if (t == "confirm") label = L"确认切窗（松开 Alt）";
                else if (t == "cancel") label = L"取消切窗";
                else if (t == "activateWindow") label = L"本地激活窗口";
                else if (t == "listWindows") label = L"列出窗口台账";
                else if (t == "runActionRecipe") label = L"按配方批量执行";
                if (label) {
                    logFn(std::wstring(L"    · ") + label);
                    ++shown;
                    continue;
                }
            }
            auto built = BuildScriptActionFromJson(params);
            if (built.ok) {
                logFn(L"    · " + ActionName(built.action));
            } else if (params.contains("type") && params["type"].is_string()) {
                logFn(L"    · " + JsonTypeBriefLabel(FromUtf8(params["type"].get<std::string>()))
                    + L"（未解析：" + TruncateForLog(built.error, 60) + L"）");
            } else {
                logFn(L"    · （无效动作项）");
            }
            ++shown;
        }
    } catch (...) {
        logFn(headline + L"（JSON 解析失败）");
    }
}




}  // namespace



bool IsAgentErrorResponse(const std::wstring& text) {

    return !text.empty() && text.rfind(L"[错误]", 0) == 0;

}



AgentSendCallbacks MakeAiMacroSendCallbacks(AiMacroLogFn logFn, const std::atomic_bool* cancelFlag,
    AiHttpAbortSlot* httpAbort) {

    AgentSendCallbacks cb;

    cb.cancelFlag = cancelFlag;
    cb.httpAbort = httpAbort;

    if (!logFn) return cb;



    auto thinkingStarted = std::make_shared<bool>(false);
    auto reasoningBuf = std::make_shared<std::wstring>();
    auto lastReasoningLog = std::make_shared<std::chrono::steady_clock::time_point>();
    auto contentStarted = std::make_shared<bool>(false);
    auto contentBuf = std::make_shared<std::wstring>();

    cb.onStatus = [logFn](const std::wstring& status) {
        if (!status.empty()) logFn(L"  " + status);
    };

    cb.onReasoningDelta = [logFn, thinkingStarted, reasoningBuf, lastReasoningLog](const std::wstring& delta) {

        if (delta.empty()) return;

        if (!*thinkingStarted) {

            *thinkingStarted = true;

            logFn(L"  思考中…");

            *lastReasoningLog = std::chrono::steady_clock::now();

        }

        *reasoningBuf += delta;

        const auto now = std::chrono::steady_clock::now();

        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - *lastReasoningLog).count();

        if (elapsed >= 3 || reasoningBuf->size() >= 160) {

            logFn(L"  思考片段：" + TruncateForLog(*reasoningBuf, 200));

            reasoningBuf->clear();

            *lastReasoningLog = now;

        }

    };

    cb.onReasoning = [logFn, thinkingStarted, reasoningBuf](const std::wstring& reasoning) {

        if (reasoning.empty()) return;

        if (!*thinkingStarted) logFn(L"  思考中…");

        *thinkingStarted = true;

        if (!reasoningBuf->empty()) {

            logFn(L"  思考片段：" + TruncateForLog(*reasoningBuf, 200));

            reasoningBuf->clear();

        }

        logFn(L"  思考过程：" + TruncateForLog(reasoning, 400));

    };

    cb.onContentDelta = [logFn, contentStarted, contentBuf](const std::wstring& delta) {
        if (delta.empty()) return;
        if (!*contentStarted) {
            *contentStarted = true;
            logFn(L"  生成回复中…");
        }
        // 禁止逐 token「回复：6」「回复：,」刷屏；汇总后由 RunVisionMessage 一次打印
        *contentBuf += delta;
    };

    cb.onToolCall = [logFn](const std::wstring& name, const std::wstring& args) {
        if (!logFn) return;
        if (name == L"submitMacroActions") {
            logFn(L"  调用工具：submitMacroActions（批量提交）");
            try {
                const nlohmann::json j = nlohmann::json::parse(ToUtf8(args));
                if (j.contains("observeAfter")) {
                    bool obs = true;
                    const auto& v = j["observeAfter"];
                    if (v.is_boolean()) obs = v.get<bool>();
                    else if (v.is_number_integer()) obs = v.get<int>() != 0;
                    logFn(obs ? L"    验收：执行后截屏观察"
                        : L"    验收：observeAfter=false（同轮继续，暂不截屏）");
                }
                if (j.contains("actions") && j["actions"].is_array()) {
                    LogSubmittedActionsBrief(logFn, L"    计划执行 ",
                        FromUtf8(j["actions"].dump()));
                }
            } catch (...) {
                logFn(L"    参数=" + TruncateForLog(args, 160));
            }
            return;
        }
        if (name == L"locateAndClick") {
            logFn(L"  调用工具：locateAndClick（zoom-refine 精确定位点击）");
            try {
                const nlohmann::json j = nlohmann::json::parse(ToUtf8(args));
                if (j.contains("target") && j["target"].is_string()) {
                    logFn(L"    目标：" + TruncateForLog(
                        FromUtf8(j["target"].get<std::string>()), 120));
                }
            } catch (...) {
                logFn(L"    参数=" + TruncateForLog(args, 160));
            }
            return;
        }
        if (name == L"activateWindow") {
            logFn(L"  调用工具：activateWindow（本地激活窗口）");
            try {
                const nlohmann::json j = nlohmann::json::parse(ToUtf8(args));
                if (j.contains("match") && j["match"].is_string()) {
                    logFn(L"    匹配：" + TruncateForLog(
                        FromUtf8(j["match"].get<std::string>()), 80));
                }
            } catch (...) {
                logFn(L"    参数=" + TruncateForLog(args, 160));
            }
            return;
        }
        if (name == L"listWindows") {
            logFn(L"  调用工具：listWindows（本地窗口台账）");
            return;
        }
        if (name == L"runActionRecipe") {
            logFn(L"  调用工具：runActionRecipe（配方批量，本地展开）");
            try {
                const nlohmann::json j = nlohmann::json::parse(ToUtf8(args));
                size_t steps = 0, rows = 0;
                if (j.contains("steps") && j["steps"].is_array()) steps = j["steps"].size();
                if (j.contains("rows") && j["rows"].is_array()) rows = j["rows"].size();
                logFn(L"    模板 " + std::to_wstring(steps) + L" 步 × "
                    + std::to_wstring(rows) + L" 组 = 约 "
                    + std::to_wstring(steps * rows) + L" 个动作（中间不识图）");
            } catch (...) {
                logFn(L"    参数=" + TruncateForLog(args, 160));
            }
            return;
        }
        if (IsMacroActionRunToolName(name) && name != L"submitMacroActions") {
            logFn(L"  调用工具：" + name + L"（规范动作）");
            try {
                nlohmann::json j = nlohmann::json::parse(ToUtf8(args));
                if (!j.is_object()) j = nlohmann::json::object();
                j["type"] = ToUtf8(name);
                LogSubmittedActionsBrief(logFn, L"    计划执行 ",
                    FromUtf8(nlohmann::json::array({ j }).dump()));
            } catch (...) {
                logFn(L"    参数=" + TruncateForLog(args, 160));
            }
            return;
        }
        if (name == L"lookupMacroAction") {
            std::wstring query;
            try {
                const nlohmann::json j = nlohmann::json::parse(ToUtf8(args));
                if (j.contains("type") && j["type"].is_string())
                    query = FromUtf8(j["type"].get<std::string>());
            } catch (...) {}
            logFn(query.empty()
                ? L"  调用工具：lookupMacroAction"
                : (L"  调用工具：lookupMacroAction → " + query));
            return;
        }
        if (name == L"completeTask") {
            std::wstring reason;
            try {
                const nlohmann::json j = nlohmann::json::parse(ToUtf8(args));
                if (j.contains("reason") && j["reason"].is_string())
                    reason = FromUtf8(j["reason"].get<std::string>());
            } catch (...) {}
            logFn(reason.empty()
                ? L"  调用工具：completeTask（任务结束）"
                : (L"  调用工具：completeTask — " + TruncateForLog(reason, 120)));
            return;
        }
        logFn(L"  调用工具：" + name + L" 参数=" + TruncateForLog(args, 160));
    };

    cb.onToolResult = [logFn](const std::wstring& name, const std::wstring& result) {
        if (!logFn || !IsMacroExecutionToolName(name)) return;

        if (name == L"completeTask") {
            logFn(L"  工具返回：任务已标记完成 " + TruncateForLog(result, 120));
            return;
        }
        if (name == L"lookupMacroAction") {
            logFn(L"  工具返回：Skill/参数说明 " + TruncateForLog(result, 160));
            return;
        }

        if (result.rfind(L"[错误]", 0) == 0) {
            logFn(L"  工具返回错误：" + TruncateForLog(result, 160));
            return;
        }

        if (result.rfind(L"[EXECUTED]", 0) == 0) {
            if (IsSubmitSkipObserveToolResult(result))
                logFn(L"  工具返回：本批动作已即时执行（同轮继续，跳过截屏）");
            else
                logFn(L"  工具返回：本批动作已即时执行（将截屏验收）");
            LogSubmittedActionsBrief(logFn, L"    已执行 ", result);
            return;
        }

        const std::wstring arr = ExtractJsonArrayFromText(result);
        if (arr.empty()) {
            logFn(L"  工具返回：非动作 JSON");
            return;
        }
        LogSubmittedActionsBrief(logFn, L"  工具返回动作 ", arr);
    };

    return cb;

}



std::string BitmapToBase64Jpeg(HBITMAP hBitmap, int quality, double scale) {
    if (!hBitmap) return {};
    try {
        cv::Mat mat = MatFromBitmap(hBitmap);
        if (mat.empty()) return {};

        // 允许上采样（ReGround 小裁剪放大）；过大仍限制
        scale = std::clamp(scale, 0.05, 8.0);

        if (std::abs(scale - 1.0) > 0.001) {
            const int interp = (scale < 1.0) ? cv::INTER_AREA : cv::INTER_LINEAR;
            cv::Mat resized;
            cv::resize(mat, resized, cv::Size(), scale, scale, interp);
            mat = std::move(resized);
        }

        std::vector<uint8_t> encoded;
        std::vector<int> params;
        params.push_back(cv::IMWRITE_JPEG_QUALITY);
        params.push_back(std::clamp(quality, 10, 100));
        if (!cv::imencode(".jpg", mat, encoded, params)) return {};
        return Base64Encode(encoded);
    } catch (...) {
        return {};
    }
}

double ComputeEffectiveAiImageScale(int width, int height, double userScale, int maxLongEdge) {
    userScale = std::clamp(userScale, 0.1, 1.0);
    if (width <= 0 || height <= 0) return userScale;
    // 观察帧默认 1024；locate 可传更高边长以保留任务栏/小图标细节（通用，非专项场景）
    maxLongEdge = std::clamp(maxLongEdge, 512, 1600);
    const double longAfterUser = static_cast<double>(std::max(width, height)) * userScale;
    if (longAfterUser <= static_cast<double>(maxLongEdge)) return userScale;
    return userScale * static_cast<double>(maxLongEdge) / longAfterUser;
}

AiImageEncodeResult EncodeBitmapForAiAnalysis(HBITMAP hBitmap, double userScale,
    int maxLongEdge, const std::wstring* imeStatusText) {
    AiImageEncodeResult out;
    if (!hBitmap) return out;
    BITMAP bm{};
    if (!GetObject(hBitmap, sizeof(bm), &bm) || bm.bmWidth <= 0 || bm.bmHeight <= 0) return out;
    out.srcWidth = bm.bmWidth;
    out.srcHeight = bm.bmHeight;
    out.effectiveScale = ComputeEffectiveAiImageScale(bm.bmWidth, bm.bmHeight, userScale, maxLongEdge);
    out.outWidth = std::max(1, static_cast<int>(bm.bmWidth * out.effectiveScale));
    out.outHeight = std::max(1, static_cast<int>(bm.bmHeight * out.effectiveScale));
    // 输入法状态画到截图左上角：TSF 输入法的候选框/组字框是 DirectComposition 独立
    // 悬浮窗，BitBlt+CAPTUREBLT 物理上拍不到；画上去等于把输入法状态烙进截图，
    // Agent 看图即知当前是中文还是英文、是否正在组字（免它盲猜/乱按快捷键）。
    // 只在副本上画，不污染原 bmp——原图另被 CommitSavedImage 存为 diff baseline，
    // 若直接改原图，IME 文字每次都会被算成「界面已变」导致每轮都重传烧 token。
    if (imeStatusText && !imeStatusText->empty()) {
        HBITMAP copy = static_cast<HBITMAP>(CopyImage(hBitmap, IMAGE_BITMAP, 0, 0, 0));
        if (copy) {
            DrawImeStatusOverlay(copy, *imeStatusText);
            out.base64 = BitmapToBase64Jpeg(copy, 62, out.effectiveScale);
            DeleteBitmapHandle(copy);
            return out;
        }
    }
    // 观察帧质量 62：小控件文字（输入法候选框/任务栏/表格表头）比 48 更清晰；体积增幅有限
    out.base64 = BitmapToBase64Jpeg(hBitmap, 62, out.effectiveScale);
    return out;
}

std::vector<std::string> EncodeClipboardSnapshotImages(const MacroClipboardSnapshot& snap) {
    std::vector<std::string> out;
    auto pushBmp = [&](HBITMAP bmp) {
        if (!bmp) return;
        const AiImageEncodeResult enc = EncodeBitmapForAiAnalysis(bmp, 1.0, 1024, nullptr);
        if (!enc.base64.empty()) out.push_back(enc.base64);
    };
    if (snap.hasBitmap) {
        HBITMAP bmp = snap.bitmap;
        bool owned = false;
        if (!bmp) {
            bmp = static_cast<HBITMAP>(DuplicateClipboardBitmapRaw());
            owned = (bmp != nullptr);
        }
        if (bmp) {
            pushBmp(bmp);
            if (owned) DeleteBitmapHandle(bmp);
        }
    }
    for (const auto& path : snap.files) {
        if (!LooksLikeImageFilePath(path)) continue;
        HBITMAP bmp = LoadBitmapFromFile(path);
        if (!bmp) continue;
        pushBmp(bmp);
        DeleteBitmapHandle(bmp);
    }
    return out;
}

AiImageEncodeResult EncodeBitmapForAiZoomUpload(HBITMAP hBitmap, int targetLongEdge) {
    AiImageEncodeResult out;
    if (!hBitmap) return out;
    BITMAP bm{};
    if (!GetObject(hBitmap, sizeof(bm), &bm) || bm.bmWidth <= 0 || bm.bmHeight <= 0) return out;
    out.srcWidth = bm.bmWidth;
    out.srcHeight = bm.bmHeight;
    targetLongEdge = std::clamp(targetLongEdge, 256, 1280);
    const int longEdge = std::max(bm.bmWidth, bm.bmHeight);
    double scale = static_cast<double>(targetLongEdge) / static_cast<double>(std::max(1, longEdge));
    if (longEdge * scale > 1280.0)
        scale = 1280.0 / static_cast<double>(longEdge);
    out.effectiveScale = scale;
    out.outWidth = std::max(1, static_cast<int>(bm.bmWidth * scale));
    out.outHeight = std::max(1, static_cast<int>(bm.bmHeight * scale));
    // locate/Zoom 略降质量：识图够用，请求体更小（观察帧已用 48）
    out.base64 = BitmapToBase64Jpeg(hBitmap, 62, scale);
    if (out.base64.empty()) {
        out.outWidth = out.outHeight = 0;
        out.effectiveScale = 1.0;
    }
    return out;
}



std::unique_ptr<AgentCore> CreateAiActionCore(

    const std::wstring& modelName,

    const std::vector<quickscript::AiModelProfile>& savedModels,

    const std::wstring& fallbackApiUrl,

    const std::wstring& fallbackApiKey,

    const std::wstring& extraSystemPrompt,

    int recvTimeoutMs,

    double temperatureOverride,

    int maxTokensOverride) {

    quickscript::AiModelProfile profile;

    profile.modelName = modelName;

    bool found = false;

    for (const auto& m : savedModels) {

        if (m.modelName == modelName) {

            profile = m;

            found = true;

            break;

        }

    }

    if (!found || profile.apiUrl.empty()) profile.apiUrl = fallbackApiUrl;

    if (!found || profile.apiKey.empty()) profile.apiKey = fallbackApiKey;



    AgentConfig cfg;

    cfg.apiUrl = profile.apiUrl;

    cfg.apiKey = profile.apiKey;

    cfg.model = profile.modelName;

    cfg.temperature = temperatureOverride >= 0.0 ? temperatureOverride : profile.temperature;

    cfg.maxTokens = maxTokensOverride > 0 ? maxTokensOverride : profile.maxTokens;

    cfg.recvTimeoutMs = std::max(5000, recvTimeoutMs);



    // 文字/图片分析也带上网页抓取能力：分析时可联网查资料（如识别出的软件快捷键）
    return std::make_unique<AgentCore>(cfg, extraSystemPrompt,
        std::vector<AgentTool>{MakeFetchWebPageTool()});

}

std::unique_ptr<AgentCore> CreateAiActionExecuteCore(

    const std::wstring& modelName,

    const std::vector<quickscript::AiModelProfile>& savedModels,

    const std::wstring& fallbackApiUrl,

    const std::wstring& fallbackApiKey,

    const std::wstring& extraSystemPrompt,

    int recvTimeoutMs,

    double temperatureOverride) {

    quickscript::AiModelProfile profile;

    profile.modelName = modelName;

    bool found = false;

    for (const auto& m : savedModels) {

        if (m.modelName == modelName) {

            profile = m;

            found = true;

            break;

        }

    }

    if (!found || profile.apiUrl.empty()) profile.apiUrl = fallbackApiUrl;

    if (!found || profile.apiKey.empty()) profile.apiKey = fallbackApiKey;



    AgentConfig cfg;

    cfg.apiUrl = profile.apiUrl;

    cfg.apiKey = profile.apiKey;

    cfg.model = profile.modelName;

    cfg.temperature = temperatureOverride >= 0.0 ? temperatureOverride : profile.temperature;

    cfg.maxTokens = profile.maxTokens > 0 ? std::min(profile.maxTokens, 2048) : 2048;

    cfg.recvTimeoutMs = std::max(5000, recvTimeoutMs);

    return std::make_unique<AgentCore>(
        cfg, extraSystemPrompt, BuildAiActionExecuteTools());

}



namespace {



std::wstring MergeAllSubmittedActionsJson(const AgentCore* core);



std::wstring ExtractSubmittedActionsJson(const AgentCore* core) {

    return MergeAllSubmittedActionsJson(core);

}



std::wstring MergeAllSubmittedActionsJson(const AgentCore* core) {

    if (!core) return L"";

    const auto& history = core->GetHistory();

    size_t lastUserIdx = 0;

    for (size_t i = history.size(); i > 0; --i) {

        if (history[i - 1].role == L"user") {

            lastUserIdx = i - 1;

            break;

        }

    }

    nlohmann::json merged = nlohmann::json::array();

    nlohmann::json stopMacroStep;

    bool hasStopMacro = false;

    int batchCount = 0;

    auto mergeJsonArray = [&](const std::wstring& text) {

        const std::wstring arr = ExtractJsonArrayFromText(text);

        if (arr.empty()) return;

        try {

            const nlohmann::json j = nlohmann::json::parse(ToUtf8(arr));

            if (!j.is_array() || j.empty()) return;

            ++batchCount;

            for (const auto& step : j) {

                if (!step.is_object()) continue;

                const std::string type = step.value("type", "");

                if (type == "stopMacro") {

                    stopMacroStep = step;

                    hasStopMacro = true;

                    continue;

                }

                merged.push_back(step);

            }

        } catch (...) {}

    };

    for (size_t i = lastUserIdx + 1; i < history.size(); ++i) {

        if (history[i].role == L"tool") {

            if (history[i].content.rfind(L"[错误]", 0) == 0) continue;

            mergeJsonArray(history[i].content);

        } else if (history[i].role == L"assistant") {

            mergeJsonArray(history[i].content);

            if (!history[i].reasoning_content.empty())

                mergeJsonArray(history[i].reasoning_content);

        }

    }

    if (hasStopMacro) merged.push_back(stopMacroStep);

    (void)batchCount;

    if (merged.empty()) return L"";

    return FromUtf8(merged.dump());

}



std::wstring BuildAiActionExecuteSkillPrompt(
    int captureWidth,
    int captureHeight,
    bool withImage) {
    // 硬规则与 Hybrid 对齐，实现集中在 ai_action_router.cpp
    return BuildAiActionToolExecuteSystemPrompt(captureWidth, captureHeight, withImage);
}



}  // namespace



AiActionResult ExecuteAiTextAnalysis(

    AgentCore* core,

    const std::wstring& resolvedPrompt,

    int /*outputType*/,

    const std::atomic_bool& stopFlag,

    int timeoutSec,

    AiMacroLogFn logFn,

    AiHttpAbortSlot* httpAbort,

    int contextMode) {

    AiActionResult result;

    if (!core) {

        result.errorMessage = L"AgentCore 未创建";

        return result;

    }

    if (stopFlag.load()) {

        result.errorMessage = L"用户取消";

        return result;

    }

    if (contextMode == 0) core->ClearHistory();



    ChatMessage msg;

    msg.role = L"user";

    msg.content = resolvedPrompt;



    AgentSendCallbacks cb = MakeAiMacroSendCallbacks(logFn, &stopFlag, httpAbort);
    cb.preferNonStream = true;
    if (core) core->SetRecvTimeoutMs(ResolveAiActionExecuteTimeoutSec(timeoutSec, false) * 1000);
    if (logFn) logFn(L"  [诊断] 上下文模式: " + std::to_wstring(contextMode));

    try {

        std::wstring response = core->SendMessage(msg, cb);

        if (stopFlag.load()) {

            result.errorMessage = L"用户取消";

            return result;

        }

        if (IsAgentErrorResponse(response)) {

            result.errorMessage = response;

            if (logFn) logFn(L"  " + TruncateForLog(response));

            return result;

        }

        result.textResult = Trim(response);

        result.ok = !result.textResult.empty();

        if (!result.ok) result.errorMessage = L"API 返回空结果";

    } catch (const std::exception& e) {

        result.errorMessage = L"API 调用异常：" + FromUtf8(e.what());

    } catch (...) {

        result.errorMessage = L"API 调用异常";

    }

    (void)timeoutSec;

    return result;

}



AiActionResult ExecuteAiImageAnalysis(

    AgentCore* core,

    const std::wstring& resolvedPrompt,

    const std::string& screenshotBase64,

    int /*outputType*/,

    const std::atomic_bool& stopFlag,

    int timeoutSec,

    AiMacroLogFn logFn,

    AiHttpAbortSlot* httpAbort,

    int contextMode,

    const std::vector<std::string>* extraImageJpegBase64) {

    AiActionResult result;

    if (!core) {

        result.errorMessage = L"AgentCore 未创建";

        return result;

    }

    if (stopFlag.load()) {

        result.errorMessage = L"用户取消";

        return result;

    }

    if (contextMode == 0) core->ClearHistory();



    ChatMessage msg;

    msg.role = L"user";

    if (!resolvedPrompt.empty()) {

        ChatContentPart textPart;

        textPart.type = L"text";

        textPart.text = resolvedPrompt;

        msg.parts.push_back(textPart);

    }

    auto appendJpeg = [&](const std::string& b64) {
        if (b64.empty()) return;
        ChatContentPart imgPart;
        imgPart.type = L"image_url";
        imgPart.image_url = L"data:image/jpeg;base64," + FromUtf8(b64);
        msg.parts.push_back(imgPart);
    };
    appendJpeg(screenshotBase64);
    if (extraImageJpegBase64) {
        for (const auto& extra : *extraImageJpegBase64) appendJpeg(extra);
    }

    const int effectiveSec = ResolveAiImageAnalysisTimeoutSec(timeoutSec, resolvedPrompt.size());
    if (core) core->SetRecvTimeoutMs(effectiveSec * 1000);

    AgentSendCallbacks cb = MakeAiMacroSendCallbacks(logFn, &stopFlag, httpAbort);
    cb.preferNonStream = false;

    if (logFn) {
        logFn(L"  [诊断] 上下文模式: " + std::to_wstring(contextMode));
        logFn(L"  超时上限 " + std::to_wstring(effectiveSec) + L"s，流式接收回复…");
        if (resolvedPrompt.size() > 400)
            logFn(L"  prompt 较长（" + std::to_wstring(resolvedPrompt.size())
                + L" 字），思考阶段可能较久");
        if (extraImageJpegBase64 && !extraImageJpegBase64->empty()) {
            logFn(L"  [诊断] 附加剪贴板图片: "
                + std::to_wstring(extraImageJpegBase64->size()) + L" 张");
        }
    }

    try {

        std::wstring response = core->SendMessage(msg, cb);

        if (stopFlag.load()) {

            result.errorMessage = L"用户取消";

            return result;

        }

        if (IsAgentErrorResponse(response)) {

            result.errorMessage = response;

            if (logFn) logFn(L"  " + TruncateForLog(response));

            return result;

        }

        result.textResult = Trim(response);

        result.ok = !result.textResult.empty();

        if (!result.ok) result.errorMessage = L"API 返回空结果";

    } catch (const std::exception& e) {

        result.errorMessage = L"API 调用异常：" + FromUtf8(e.what());

    } catch (...) {

        result.errorMessage = L"API 调用异常";

    }

    (void)timeoutSec;

    return result;

}



std::wstring BuildAiActionExecuteUserInstruction(
    const std::wstring& taskPrompt,
    int captureWidth,
    int captureHeight,
    bool withImage) {

    std::wstring s;
    if (withImage && captureWidth > 0 && captureHeight > 0) {
        s += L"截图 " + std::to_wstring(captureWidth) + L"×" + std::to_wstring(captureHeight) + L"。\n";
    }
    if (!taskPrompt.empty()) s += taskPrompt;
    return s;
}



std::wstring BuildAiActionExecuteSystemPrompt(

    int captureWidth,

    int captureHeight) {

    return BuildAiActionExecuteSkillPrompt(captureWidth, captureHeight, true);

}



std::wstring BuildAiActionExecuteTextSystemPrompt() {

    return BuildAiActionExecuteSkillPrompt(0, 0, false);

}



int ResolveAiActionExecuteTimeoutSec(int userTimeoutSec, bool withImage) {
    const int base = userTimeoutSec > 0 ? userTimeoutSec : 30;
    if (withImage) return std::max(base, 90);  // 与图片分析对齐；过长只会让用户干等
    return std::max(base, 45);
}

/// 单次识图定位：上限更短，避免「找不到」时干等 90s×2 级
int ResolveAiLocateVisionTimeoutSec(int userTimeoutSec) {
    const int base = userTimeoutSec > 0 ? userTimeoutSec : 45;
    return std::clamp(base, 20, 60);
}

int ResolveAiImageAnalysisTimeoutSec(int userTimeoutSec, size_t promptChars) {
    const int base = userTimeoutSec > 0 ? userTimeoutSec : 90;
    const int extra = static_cast<int>(std::min<size_t>(promptChars / 60, 180));
    return std::max(base + extra, 90);
}



namespace {



ChatMessage BuildAiUserMessage(const std::wstring& text, const std::string& screenshotBase64,
    const std::vector<std::string>* extraImages = nullptr) {

    ChatMessage msg;

    msg.role = L"user";

    const bool hasExtra = extraImages && !extraImages->empty();
    const bool hasAnyImage = !screenshotBase64.empty() || hasExtra;

    if (hasAnyImage) {

        if (!text.empty()) {

            ChatContentPart textPart;

            textPart.type = L"text";

            textPart.text = text;

            msg.parts.push_back(textPart);

        }

        auto appendJpeg = [&](const std::string& b64) {
            if (b64.empty()) return;
            ChatContentPart imgPart;
            imgPart.type = L"image_url";
            imgPart.image_url = L"data:image/jpeg;base64," + FromUtf8(b64);
            msg.parts.push_back(imgPart);
        };
        appendJpeg(screenshotBase64);
        if (extraImages) {
            for (const auto& extra : *extraImages) appendJpeg(extra);
        }

    } else {

        msg.content = text;

    }

    return msg;

}



AiActionResult RunVisionMessage(

    AgentCore* core,

    const ChatMessage& msg,

    int contextMode,

    const std::atomic_bool& stopFlag,

    AiMacroLogFn logFn,

    AiHttpAbortSlot* httpAbort) {

    AiActionResult result;

    if (contextMode == 0) core->ClearHistory();

    AgentSendCallbacks cb = MakeAiMacroSendCallbacks(logFn, &stopFlag, httpAbort);
    // 识图定位：完整响应更稳，避免大图流式卡在「上传中/生成中」
    cb.preferNonStream = true;
    // 单轮识图勿拖到动作执行的 90s+；找不到应尽快失败
    if (core) {
        const int prevMs = 0;
        (void)prevMs;
        core->SetRecvTimeoutMs(ResolveAiLocateVisionTimeoutSec(0) * 1000);
    }
    if (logFn) logFn(L"  [诊断] 识图请求: 完整响应（非流式）");

    try {

        const std::wstring response = core->SendMessage(msg, cb);

        if (stopFlag.load()) {

            result.errorMessage = L"用户取消";

            return result;

        }

        if (IsAgentErrorResponse(response)) {

            result.errorMessage = response;

            return result;

        }

        result.textResult = Trim(response);

        result.ok = !result.textResult.empty();

        result.visionQueryText = result.ok;

        if (!result.ok) result.errorMessage = L"API 返回空结果";
        else if (logFn)
            logFn(L"  回复：" + TruncateForLog(result.textResult, 240));

    } catch (const std::exception& e) {

        result.errorMessage = L"API 调用异常：" + FromUtf8(e.what());

    } catch (...) {

        result.errorMessage = L"API 调用异常";

    }

    return result;

}



bool JsonArrayHasStopMacro(const std::wstring& actionsJson) {

    try {

        const nlohmann::json j = nlohmann::json::parse(ToUtf8(actionsJson));

        if (!j.is_array()) return false;

        for (const auto& step : j) {

            if (step.is_object() && step.value("type", "") == "stopMacro") return true;

        }

    } catch (...) {}

    return false;

}



void LogExtractedActions(AiMacroLogFn logFn, const std::wstring& actionsJson) {
    if (!logFn || actionsJson.empty()) return;
    LogSubmittedActionsBrief(logFn, L"  [诊断] 解析到动作 ", actionsJson);
}



AiActionResult FinalizeToolActionsResult(

    AgentCore* core,

    const std::wstring& response,

    const std::atomic_bool& stopFlag,

    AiMacroLogFn logFn) {

    AiActionResult result;

    if (stopFlag.load()) {

        const std::wstring actionsJson = ExtractSubmittedActionsJson(core);

        if (!actionsJson.empty()) {

            result.textResult = actionsJson;

            result.ok = true;

            return result;

        }

        result.errorMessage = L"用户取消";

        return result;

    }

    if (response.find(L"已取消") != std::wstring::npos || response.find(L"用户取消") != std::wstring::npos) {

        const std::wstring actionsJson = ExtractSubmittedActionsJson(core);

        if (!actionsJson.empty()) {

            result.textResult = actionsJson;

            result.ok = true;

            return result;

        }

        result.errorMessage = L"用户取消";

        return result;

    }

    if (IsAgentErrorResponse(response)) {

        const std::wstring actionsJson = ExtractSubmittedActionsJson(core);

        if (!actionsJson.empty()) {

            result.textResult = actionsJson;

            result.ok = true;

            return result;

        }

        result.errorMessage = response;

        if (logFn) logFn(L"  " + TruncateForLog(response));

        return result;

    }

    std::wstring actionsJson = ExtractSubmittedActionsJson(core);

    if (actionsJson.empty()) actionsJson = ExtractJsonArrayFromText(Trim(response));

    LogExtractedActions(logFn, actionsJson);

    if (actionsJson.empty()) {

        result.textResult = Trim(response);

        result.errorMessage = L"API 未返回有效动作 JSON";

        if (logFn && !response.empty())

            logFn(L"  [诊断] 原始回复：" + TruncateForLog(Trim(response)));

        return result;

    }

    result.textResult = actionsJson;

    result.ok = true;

    return result;

}



}  // namespace



ZoomRefineLocateResult ExecuteZoomRefineLocate(
    AgentCore* core,
    const std::wstring& userTask,
    const std::string& initialScreenshotBase64,
    int initialApiW,
    int initialApiH,
    const AiCaptureMapping& rootMap,
    const std::atomic_bool& stopFlag,
    AiMacroLogFn logFn,
    AiHttpAbortSlot* httpAbort,
    ZoomRefineLocateOptions opts) {
    ZoomRefineLocateResult out;
    if (!core) {
        out.errorMessage = L"无 AI 客户端";
        return out;
    }
    if (initialScreenshotBase64.empty() || initialApiW <= 0 || initialApiH <= 0) {
        out.errorMessage = L"缺少截图";
        return out;
    }
    if (rootMap.capX2 <= rootMap.capX1 || rootMap.capY2 <= rootMap.capY1) {
        out.errorMessage = L"截图区域无效";
        return out;
    }

    const int maxLevels = std::clamp(opts.maxLevels, 1, 3);
    AiCaptureMapping curMap = rootMap;
    curMap.apiWidth = initialApiW;
    curMap.apiHeight = initialApiH;
    std::string curB64 = initialScreenshotBase64;
    int curApiW = initialApiW;
    int curApiH = initialApiH;
    int screenX = 0;
    int screenY = 0;
    bool havePriorLocate = false;
    int priorScreenX = 0;
    int priorScreenY = 0;
    int priorBoxMaxSide = 0;
    int startLevel = 0;
    int effectiveMaxLevels = maxLevels;

    auto finishWithPrior = [&](int levelsUsed, const wchar_t* why) -> ZoomRefineLocateResult {
        if (logFn) {
            logFn(L"  [诊断] " + std::wstring(why)
                + L"，沿用上级屏幕(" + std::to_wstring(priorScreenX) + L","
                + std::to_wstring(priorScreenY) + L")");
        }
        out.ok = true;
        out.screenX = priorScreenX;
        out.screenY = priorScreenY;
        out.levelsUsed = levelsUsed;
        return out;
    };

    auto TrySnapLocateByFindImage = [&](int estimateX, int estimateY, int boxW, int boxH,
        int& outX, int& outY, double& outScore) -> bool {
        int tw = boxW > 0 ? boxW : 48;
        int th = boxH > 0 ? boxH : 48;
        tw = std::clamp(tw, 20, 320);
        th = std::clamp(th, 20, 320);
        // 略扩边，避免裁到抗锯齿边缘导致匹配不稳
        const int pad = std::max(2, std::min(tw, th) / 10);
        int x1 = estimateX - tw / 2 - pad;
        int y1 = estimateY - th / 2 - pad;
        int x2 = estimateX + (tw + 1) / 2 + pad;
        int y2 = estimateY + (th + 1) / 2 + pad;
        x1 = std::max(rootMap.capX1, x1);
        y1 = std::max(rootMap.capY1, y1);
        x2 = std::min(rootMap.capX2, x2);
        y2 = std::min(rootMap.capY2, y2);
        if (x2 - x1 < 12 || y2 - y1 < 12) return false;

        HBITMAP tmpl = CaptureScreenRegion(x1, y1, x2, y2);
        if (!tmpl) return false;
        if (logFn) {
            logFn(L"  [诊断] 找图精修：模板屏幕[" + std::to_wstring(x1) + L","
                + std::to_wstring(y1) + L"," + std::to_wstring(x2) + L","
                + std::to_wstring(y2) + L"]，全屏搜索…");
        }

        ImageMatchOptions matchOpt;
        matchOpt.thresholdPercent = std::clamp(opts.findImageThreshold, 50.0, 99.0);
        matchOpt.scaleMin = 0.95;
        matchOpt.scaleMax = 1.05;
        matchOpt.scaleStep = 0.05;
        matchOpt.disablePyramid = true;
        matchOpt.maxMatches = 8;
        const ImageMatchOutput matched = FindTemplateOnScreenMulti(
            rootMap.capX1, rootMap.capY1, rootMap.capX2, rootMap.capY2,
            tmpl, matchOpt);
        DeleteBitmapHandle(tmpl);

        if (!matched.found || matched.matches.empty()) {
            if (logFn) logFn(L"  [诊断] 找图精修未命中，保留识图坐标");
            return false;
        }

        const int maxDist = opts.findImageMaxSnapDist > 0 ? opts.findImageMaxSnapDist : 280;
        const ImageMatchResult* best = nullptr;
        long long bestDist2 = -1;
        for (const auto& m : matched.matches) {
            if (!m.found || m.score < matchOpt.thresholdPercent) continue;
            int cx = 0, cy = 0;
            FindImageMatchCenter(m, cx, cy);
            const long long dx = static_cast<long long>(cx) - estimateX;
            const long long dy = static_cast<long long>(cy) - estimateY;
            const long long d2 = dx * dx + dy * dy;
            if (d2 > static_cast<long long>(maxDist) * maxDist) continue;
            if (!best || d2 < bestDist2 || (d2 == bestDist2 && m.score > best->score)) {
                best = &m;
                bestDist2 = d2;
            }
        }
        if (!best) {
            if (logFn) {
                logFn(L"  [诊断] 找图命中过远（>" + std::to_wstring(maxDist)
                    + L"px），保留识图坐标");
            }
            return false;
        }
        FindImageMatchCenter(*best, outX, outY);
        outScore = best->score;
        if (logFn) {
            logFn(L"  [诊断] 找图精修命中屏幕(" + std::to_wstring(outX) + L","
                + std::to_wstring(outY) + L") 匹配度 "
                + std::to_wstring(static_cast<int>(best->score + 0.5))
                + L"%（相对识图偏移 "
                + std::to_wstring(static_cast<int>(std::sqrt(static_cast<double>(bestDist2)) + 0.5))
                + L"px）");
        }
        return true;
    };

    // ZoomClick 风格：按粗框/收缩比裁屏，再上采样到 uploadLongEdge
    auto advanceZoomCrop = [&](int centerX, int centerY, int boxW, int boxH) -> bool {
        try {
            const int prevW = std::max(1, curMap.capX2 - curMap.capX1);
            const int prevH = std::max(1, curMap.capY2 - curMap.capY1);
            int side = 0;
            const int boxMax = std::max(boxW, boxH);
            // 小图标：紧 ROI，避免把相邻相似控件一起塞进 Zoom（×4 上采样后更易点错邻居）
            if (boxMax > 0 && boxMax <= 56) {
                const double pad = 2.0;
                side = static_cast<int>(boxMax * pad);
                side = std::clamp(side, 80, 200);
            } else if (boxW > 0 && boxH > 0) {
                side = static_cast<int>(boxMax * std::max(1.5, opts.bboxPadFactor));
                side = std::clamp(side, opts.minRoiSide, opts.maxRoiSide);
            } else {
                side = static_cast<int>(std::min(prevW, prevH) * std::clamp(opts.shrinkRatio, 0.25, 0.85));
                side = std::clamp(side, opts.minRoiSide, opts.maxRoiSide);
            }
            const int half = std::max(1, side / 2);
            int roiX1 = 0, roiY1 = 0, roiX2 = 0, roiY2 = 0;
            BuildZoomRoiAroundScreenPoint(centerX, centerY, half,
                rootMap.capX1, rootMap.capY1, rootMap.capX2, rootMap.capY2,
                roiX1, roiY1, roiX2, roiY2);
            if (logFn) {
                logFn(L"  [诊断] 正在裁剪放大区 屏幕["
                    + std::to_wstring(roiX1) + L"," + std::to_wstring(roiY1) + L","
                    + std::to_wstring(roiX2) + L"," + std::to_wstring(roiY2) + L"]…");
            }
            HBITMAP crop = CaptureScreenRegion(roiX1, roiY1, roiX2, roiY2);
            if (!crop) {
                if (logFn) logFn(L"  [诊断] Zoom 截屏失败");
                return false;
            }
            const int uploadEdge = opts.uploadLongEdge > 0 ? opts.uploadLongEdge : 768;
            const AiImageEncodeResult enc = EncodeBitmapForAiZoomUpload(crop, uploadEdge);
            DeleteBitmapHandle(crop);
            if (enc.base64.empty()) {
                if (logFn) logFn(L"  [诊断] Zoom 上采样/编码失败");
                return false;
            }
            if (logFn) {
                logFn(L"  [诊断] Zoom ROI 屏幕[" + std::to_wstring(roiX1) + L","
                    + std::to_wstring(roiY1) + L"," + std::to_wstring(roiX2) + L","
                    + std::to_wstring(roiY2) + L"] 裁切 "
                    + std::to_wstring(enc.srcWidth) + L"×" + std::to_wstring(enc.srcHeight)
                    + L" → 上采样上传 " + std::to_wstring(enc.outWidth) + L"×"
                    + std::to_wstring(enc.outHeight)
                    + L"（×" + std::to_wstring(enc.effectiveScale) + L"）");
            }
            curB64 = enc.base64;
            curApiW = enc.outWidth;
            curApiH = enc.outHeight;
            curMap.capX1 = roiX1;
            curMap.capY1 = roiY1;
            curMap.capX2 = roiX2;
            curMap.capY2 = roiY2;
            curMap.srcWidth = enc.srcWidth;
            curMap.srcHeight = enc.srcHeight;
            curMap.apiWidth = enc.outWidth;
            curMap.apiHeight = enc.outHeight;
            return true;
        } catch (...) {
            if (logFn) logFn(L"  [诊断] Zoom 裁切异常，跳过放大");
            return false;
        }
    };

    // Midscene 对齐：短标签 + 一级整图 VLM
    const std::wstring locatePhrase = ExtractClickTargetPhrase(userTask);
    if (logFn && locatePhrase != Trim(userTask) && !locatePhrase.empty()) {
        logFn(L"  [诊断] 定位短语缩短：「" + locatePhrase + L"」（原文过长/含引号）");
    }

    for (int level = startLevel; level < effectiveMaxLevels; ++level) {
        if (stopFlag.load()) {
            out.errorMessage = L"用户取消";
            return out;
        }
        std::wstring prompt;
        // 一级：整图 Locate；二级+：Zoom 裁剪后 Refine（deepLocate）
        if (level == 0) {
            prompt = BuildCompositeLocatePrompt(
                locatePhrase.empty() ? userTask : locatePhrase, curApiW, curApiH);
        } else {
            prompt = BuildCompositeRefinePointPrompt(
                locatePhrase.empty() ? userTask : locatePhrase, level, curApiW, curApiH);
        }
        if (logFn) {
            logFn(L"  [诊断] locate 第 " + std::to_wstring(level + 1) + L"/"
                + std::to_wstring(effectiveMaxLevels) + L" 级，图 "
                + std::to_wstring(curApiW) + L"×" + std::to_wstring(curApiH)
                + (level == 0 ? L"（整图 VLM）" : L"（Zoom deepLocate）"));
        }
        const ChatMessage msg = BuildAiUserMessage(prompt, curB64);
        AiActionResult vision = RunVisionMessage(
            core, msg, 0, stopFlag, logFn, httpAbort);
        if (!vision.ok) {
            if (havePriorLocate)
                return finishWithPrior(level, L"本级识图失败");
            out.errorMessage = vision.errorMessage.empty()
                ? L"识图定位失败" : vision.errorMessage;
            return out;
        }

        // 模型明确说找不到：Zoom 级回退上级中心（粗点仍可能可用）；一级则失败
        if (IsVisionLocateNotFound(vision.textResult)) {
            if (logFn) {
                logFn(L"  [诊断] 识图判定目标不在画面："
                    + TruncateForLog(vision.textResult, 80));
            }
            if (level > 0 && havePriorLocate) {
                // 上级粗框过大时中心几乎必错：Zoom 未确认则拒绝盲点（勿点列表中央）
                if (priorBoxMaxSide >= 160) {
                    if (logFn) {
                        logFn(L"  [诊断] Zoom 级 NOT_FOUND，上级粗框过大(maxSide="
                            + std::to_wstring(priorBoxMaxSide) + L")，拒绝盲点");
                    }
                    out.errorMessage = L"未找到目标（Zoom 未确认且上级框过大）。"
                        L"换更具体短标签/方位再 locate，勿猜中心点。";
                    return out;
                }
                if (logFn)
                    logFn(L"  [诊断] Zoom 级 NOT_FOUND，回退上级粗点（紧 ROI 或换描述）");
                return finishWithPrior(level, L"Zoom 级 NOT_FOUND");
            }
            out.errorMessage = L"未找到目标（识图返回 NOT_FOUND）"
                L"。可 scrollWheel/PageDown 露出更多内容后再 locate，或换描述，勿猜坐标。";
            return out;
        }

        int apiX = 0, apiY = 0;
        int bx1 = 0, by1 = 0, bx2 = 0, by2 = 0;
        const bool haveBox = TryParseBoundingBox(vision.textResult, bx1, by1, bx2, by2);
        int newSx = 0, newSy = 0;
        int boxW = 0, boxH = 0;
        int screenBoxX1 = 0, screenBoxY1 = 0, screenBoxX2 = 0, screenBoxY2 = 0;
        bool haveScreenBox = false;
        std::wstring coordNote;
        bool coordsWereRemapped = false;

        if (haveBox) {
            if (!ResolveVisionRectToApiImage(bx1, by1, bx2, by2,
                    curApiW, curApiH, curMap.srcWidth, curMap.srcHeight, &coordNote)) {
                if (havePriorLocate)
                    return finishWithPrior(level, L"本级矩形坐标越界（疑似幻觉）");
                out.errorMessage = L"定位矩形越出截图范围："
                    + TruncateForLog(vision.textResult, 80);
                return out;
            }
            if (level == 0
                && IsVisionApiBoxTooLarge(bx1, by1, bx2, by2, curApiW, curApiH, 0.22)) {
                if (logFn) {
                    logFn(L"  [诊断] 粗框面积过大（疑似乱框），拒绝："
                        + std::to_wstring(bx1) + L"," + std::to_wstring(by1) + L","
                        + std::to_wstring(bx2) + L"," + std::to_wstring(by2));
                }
                if (havePriorLocate)
                    return finishWithPrior(level, L"本级粗框过大");
                out.errorMessage = L"未找到目标（定位框过大，疑似未命中）";
                return out;
            }
            coordsWereRemapped = (coordNote != L"上传图像素");
            MapApiRectToScreen(curMap, bx1, by1, bx2, by2,
                screenBoxX1, screenBoxY1, screenBoxX2, screenBoxY2);
            haveScreenBox = true;
            newSx = (screenBoxX1 + screenBoxX2) / 2;
            newSy = (screenBoxY1 + screenBoxY2) / 2;
            boxW = std::max(1, screenBoxX2 - screenBoxX1);
            boxH = std::max(1, screenBoxY2 - screenBoxY1);
            if (logFn) {
                logFn(L"  [诊断] 第" + std::to_wstring(level + 1)
                    + L"级矩形 api[" + std::to_wstring(bx1) + L"," + std::to_wstring(by1)
                    + L"," + std::to_wstring(bx2) + L"," + std::to_wstring(by2)
                    + L"]（" + coordNote + L"）→ 屏幕中心(" + std::to_wstring(newSx) + L","
                    + std::to_wstring(newSy) + L")");
            }
        } else {
            if (!TryParseCoordinatePair(vision.textResult, apiX, apiY)) {
                if (havePriorLocate)
                    return finishWithPrior(level, L"本级无法解析坐标");
                out.errorMessage = L"无法解析定位结果：" + TruncateForLog(vision.textResult, 80);
                return out;
            }
            if (!ResolveVisionPointToApiImage(apiX, apiY,
                    curApiW, curApiH, curMap.srcWidth, curMap.srcHeight, &coordNote)) {
                if (havePriorLocate)
                    return finishWithPrior(level, L"本级坐标越界（疑似幻觉）");
                out.errorMessage = L"定位坐标越出截图范围";
                return out;
            }
            coordsWereRemapped = (coordNote != L"上传图像素");
            // Zoom deepLocate 精点若落在图正中心附近：模型常偷懒回中心，回退上级
            if (level > 0 && havePriorLocate && curApiW > 8 && curApiH > 8) {
                const int deadX = std::max(8, curApiW / 10);
                const int deadY = std::max(8, curApiH / 10);
                if (std::abs(apiX - curApiW / 2) <= deadX
                    && std::abs(apiY - curApiH / 2) <= deadY) {
                    return finishWithPrior(level, L"精点落在放大区中心（疑似偷懒）");
                }
            }
            MapApiPointToScreen(curMap, apiX, apiY, newSx, newSy);
            if (logFn) {
                logFn(L"  [诊断] 第" + std::to_wstring(level + 1)
                    + L"级点 api(" + std::to_wstring(apiX) + L"," + std::to_wstring(apiY)
                    + L")（" + coordNote + L"）→ 屏幕(" + std::to_wstring(newSx) + L","
                    + std::to_wstring(newSy) + L")");
            }
        }

        // 映射后仍须落在截屏观察区内（防止 y=1956 这类飞点）
        if (newSx < rootMap.capX1 || newSy < rootMap.capY1
            || newSx >= rootMap.capX2 || newSy >= rootMap.capY2) {
            if (havePriorLocate)
                return finishWithPrior(level, L"本级映射出观察区");
            out.errorMessage = L"定位结果映射到观察区外("
                + std::to_wstring(newSx) + L"," + std::to_wstring(newSy) + L")";
            return out;
        }

        if (havePriorLocate && level > 0) {
            int driftCap = opts.maxRefineDriftPx > 0 ? opts.maxRefineDriftPx : 220;
            // 粗框是小图标时：邻居常在 40~80px 内，收紧漂移以免采纳错邻居
            if (priorBoxMaxSide > 0 && priorBoxMaxSide <= 56)
                driftCap = (std::min)(driftCap, 72);
            if (IsRefineScreenDriftTooFar(priorScreenX, priorScreenY, newSx, newSy, driftCap)) {
                return finishWithPrior(level, L"本级相对上级漂移过大");
            }
        }

        screenX = newSx;
        screenY = newSy;
        priorScreenX = newSx;
        priorScreenY = newSy;
        if (haveScreenBox)
            priorBoxMaxSide = (std::max)(boxW, boxH);
        else if (level == 0)
            priorBoxMaxSide = 0;
        havePriorLocate = true;
        out.levelsUsed = level + 1;

        // 自适应 refine：紧凑/宽控件粗框 / 归一化单点可直接点，省一轮 API（对齐 Midscene deepLocate 按需）
        {
            const int capW = (std::max)(1, rootMap.capX2 - rootMap.capX1);
            const int capH = (std::max)(1, rootMap.capY2 - rootMap.capY1);
            CoarseLocateRefineGateInput gate;
            gate.haveScreenBox = haveScreenBox;
            gate.pointOnly = !haveScreenBox;
            gate.boxW = boxW;
            gate.boxH = boxH;
            gate.captureW = capW;
            gate.captureH = capH;
            gate.coordsWereRemapped = coordsWereRemapped;
            gate.acceptCompactBboxWithoutRefine = opts.acceptCompactBboxWithoutRefine;
            gate.adaptiveRefineDepth = opts.adaptiveRefineDepth;
            gate.compactBboxMinSide = opts.compactBboxMinSide;
            gate.compactBboxMaxSide = opts.compactBboxMaxSide;
            gate.compactAreaRatioMax = opts.compactAreaRatioMax;
            gate.compactAspectMin = opts.compactAspectMin;
            gate.compactAspectMax = opts.compactAspectMax;
            CoarseLocateSkipReason why = CoarseLocateSkipReason::None;
            if (level == 0 && ShouldAcceptCoarseLocateWithoutRefine(gate, &why)) {
                const wchar_t* whyText = L"紧凑框";
                if (why == CoarseLocateSkipReason::WideControlRemapped) whyText = L"宽控件";
                else if (why == CoarseLocateSkipReason::CompactRemapped) whyText = L"归一化紧凑框";
                else if (why == CoarseLocateSkipReason::CompactPixel) whyText = L"像素紧凑框";
                else if (why == CoarseLocateSkipReason::PointRemapped) whyText = L"归一化单点";
                if (logFn) {
                    if (why == CoarseLocateSkipReason::PointRemapped) {
                        logFn(L"  [诊断] 一级可点(归一化单点)，跳过二级 refine（省一轮 API）");
                    } else {
                        logFn(L"  [诊断] 一级可点(" + std::wstring(whyText) + L" "
                            + std::to_wstring(boxW) + L"×" + std::to_wstring(boxH)
                            + L")，跳过二级 refine（省一轮 API）");
                    }
                }
                out.ok = true;
                out.screenX = screenX;
                out.screenY = screenY;
                out.skippedRefine = true;
                return out;
            }
        }

        // 懒补级：调用方只请求 1 级，但粗框未过紧凑门禁（偏大/模糊）→ 自动补一级 Zoom 精炼。
        // 简单目标保持 1 轮 API；难目标仍能拿到 2 级精度（对齐 Midscene deepLocate 按需）。
        if (level == 0 && effectiveMaxLevels == 1
            && opts.lazyEscalateRefine && opts.lazyEscalateMaxLevels > 1) {
            effectiveMaxLevels = std::clamp(opts.lazyEscalateMaxLevels, 2, 3);
            if (logFn) {
                logFn(L"  [诊断] 一级粗框未达「紧凑即点」，lazy 补一级 Zoom 精炼"
                    L"（简单目标仍仅 1 轮 API）");
            }
        }

        const bool lastLevel = (level + 1 >= effectiveMaxLevels);
        if (level == 0 && coordsWereRemapped && !lastLevel && logFn) {
            logFn(L"  [诊断] 坐标经" + coordNote + L"换算，继续放大精炼");
        }
        // 找图精修放在末级之后：避免同帧自匹配 100% 跳过 Zoom 固化错误粗点
        if (opts.snapByFindImage && lastLevel) {
            if (stopFlag.load()) {
                out.errorMessage = L"用户取消";
                return out;
            }
            int snapX = screenX, snapY = screenY;
            double snapScore = -1.0;
            if (TrySnapLocateByFindImage(screenX, screenY, boxW, boxH,
                    snapX, snapY, snapScore)) {
                screenX = snapX;
                screenY = snapY;
                priorScreenX = snapX;
                priorScreenY = snapY;
                out.usedFindImageSnap = true;
                out.findImageScore = snapScore;
            }
            out.ok = true;
            out.screenX = screenX;
            out.screenY = screenY;
            return out;
        }

        if (lastLevel) {
            out.ok = true;
            out.screenX = screenX;
            out.screenY = screenY;
            return out;
        }

        if (stopFlag.load()) {
            out.errorMessage = L"用户取消";
            return out;
        }
        // Midscene deepLocate：一级未过紧凑门禁 → 围绕中心 Zoom 精点（不再走全图纠偏）
        if (!advanceZoomCrop(screenX, screenY, boxW, boxH)) {
            if (logFn) logFn(L"  [诊断] Zoom deepLocate 裁剪失败，尝试找图后点击");
            if (opts.snapByFindImage) {
                int snapX = screenX, snapY = screenY;
                double snapScore = -1.0;
                if (TrySnapLocateByFindImage(screenX, screenY, boxW, boxH,
                        snapX, snapY, snapScore)) {
                    screenX = snapX;
                    screenY = snapY;
                    out.usedFindImageSnap = true;
                    out.findImageScore = snapScore;
                }
            }
            out.ok = true;
            out.screenX = screenX;
            out.screenY = screenY;
            return out;
        }
    }

    out.ok = havePriorLocate;
    out.screenX = screenX;
    out.screenY = screenY;
    if (out.ok && opts.snapByFindImage && !out.usedFindImageSnap) {
        int snapX = screenX, snapY = screenY;
        double snapScore = -1.0;
        if (TrySnapLocateByFindImage(screenX, screenY, 48, 48, snapX, snapY, snapScore)) {
            out.screenX = snapX;
            out.screenY = snapY;
            out.usedFindImageSnap = true;
            out.findImageScore = snapScore;
        }
    }
    if (!out.ok) out.errorMessage = L"定位未得到有效坐标";
    return out;
}

bool VerifyClickEffectByColorSample(
    int screenX, int screenY,
    int beforeR, int beforeG, int beforeB,
    int tolerance) {
    Sleep(120);
    int r = 0, g = 0, b = 0;
    if (!GetScreenPixelRgb(screenX, screenY, r, g, b)) return false;
    return !ColorsMatch(beforeR, beforeG, beforeB, r, g, b, tolerance);
}

AiActionResult ExecuteAiActionExecute(
    AgentCore* core,
    const std::wstring& resolvedPrompt,
    const std::string& screenshotBase64,
    int captureWidth,
    int captureHeight,
    int contextMode,
    const std::atomic_bool& stopFlag,
    int timeoutSec,
    AiMacroLogFn logFn,
    AiHttpAbortSlot* httpAbort,
    const AiCaptureMapping* captureMapping,
    AiActionHostHooks* hostHooks,
    int maxAgentRounds,
    const std::vector<std::string>* extraImageJpegBase64) {

    AiActionResult result;
    if (!core) {
        result.errorMessage = L"AgentCore 未创建";
        return result;
    }
    if (stopFlag.load()) {
        result.errorMessage = L"用户取消";
        return result;
    }

    const bool withImage = !screenshotBase64.empty();
    const AiActionRouteKind route = ClassifyAiActionRoute(resolvedPrompt, withImage);
    result.routeKind = route;
    const int effectiveTimeoutSec = ResolveAiActionExecuteTimeoutSec(timeoutSec, withImage);
    core->SetRecvTimeoutMs(effectiveTimeoutSec * 1000);

    const bool agentic = hostHooks
        && hostHooks->onExecuteActions
        && (route == AiActionRouteKind::ToolExecute || route == AiActionRouteKind::MultiTurnTools);
    const bool fillTableOnly = resolvedPrompt.find(L"只填表") != std::wstring::npos
        || resolvedPrompt.find(L"动态数据·只填表") != std::wstring::npos
        || resolvedPrompt.find(L"动态列表填写") != std::wstring::npos;
    if (route == AiActionRouteKind::ToolExecute || route == AiActionRouteKind::MultiTurnTools) {
        AiActionToolOptions opts;
        // Computer Use：坐标相对所见截图；无图时绝对点无映射，硬拒
        opts.allowAbsolutePointer = withImage;
        opts.fillTableOnly = fillTableOnly;
        core->UpdateTools(BuildAiActionExecuteTools(agentic ? hostHooks : nullptr, opts));
        if (fillTableOnly && logFn)
            logFn(L"  [诊断] 动态填表工具白名单：禁 runProgram/openWebpage/hotkey 等");
    }
    if (agentic) {
        SetAiActionPlanGateEnabled(true);
    }

    if (logFn) {
        logFn(L"  [诊断] 路由: " + AiActionRouteLabel(route));
        logFn(L"  [诊断] 用户消息含图片: " + std::wstring(withImage ? L"是" : L"否"));
        if (withImage)
            logFn(L"  [诊断] base64字节数: " + std::to_wstring(screenshotBase64.size()));
        if (extraImageJpegBase64 && !extraImageJpegBase64->empty()) {
            logFn(L"  [诊断] 附加剪贴板图片: "
                + std::to_wstring(extraImageJpegBase64->size()) + L" 张");
        }
        logFn(L"  [诊断] API 超时: " + std::to_wstring(effectiveTimeoutSec) + L"s");
        logFn(L"  [诊断] 上下文模式: " + std::to_wstring(contextMode));
        logFn(L"  [诊断] Agent闭环: " + std::wstring(agentic ? L"是" : L"否"));
        if (route == AiActionRouteKind::CompositeClick
            || route == AiActionRouteKind::VisionQuery) {
            logFn(L"  [诊断] 传输: 完整响应（识图定位，避免大图流式卡上传）");
        } else {
            // 工具路径优先流式：思考模型可尽早吐字/调工具；失败再回落完整响应
            logFn(L"  [诊断] 传输: 优先流式（失败则完整响应）");
        }
        logFn(L"  [诊断] 工具轮: thinking=disabled（避免只想不调工具）");
        const auto& hist = core->GetHistory();
        if (!hist.empty() && hist[0].role == L"system") {
            logFn(L"  [诊断] system 提示 " + std::to_wstring(hist[0].content.size())
                + L" 字（短指针；Skill 按需 lookup）");
        }
    }

    try {
        if (route == AiActionRouteKind::CompositeClick) {
            // 本地编排：直接跑 locateAndClick 管线，不经 Agent 规划轮；默认 1 次识图。
            if (hostHooks && hostHooks->onLocateAndClick) {
                if (logFn) {
                    logFn(L"  [诊断] 本地调用 locateAndClick（ReGround：粗定位+放大精点）");
                }
                // 只把点击目标短语交给定位（剥「点击/请帮我」等动作前缀、截断到逗号），
                // 避免整段任务原文塞进识图 prompt 烧 token / 让模型误认多个目标。
                const std::wstring target = ExtractClickTargetPhrase(resolvedPrompt);
                const std::wstring button = PromptIntendsRightClick(resolvedPrompt)
                    ? L"right" : L"left";
                const int clickCount = PromptIntendsDoubleClick(resolvedPrompt) ? 2 : 1;
                if (logFn) {
                    logFn(L"  [诊断] 目标短语:「" + target + L"」 按键:" + button
                        + (clickCount > 1 ? L" 双击" : L" 单击"));
                }
                // 快路径：默认 1 级识图；简单目标「紧凑即点」跳过二级（省一轮 API），
                // 粗框偏大/模糊时 lazy 补级自动升到 2（自适应精炼）。
                const std::wstring msg = hostHooks->onLocateAndClick(
                    target, 1, button, clickCount);
                result.routeKind = route;
                if (msg.rfind(L"[错误]", 0) == 0) {
                    result.ok = false;
                    result.errorMessage = msg;
                    return result;
                }
                result.ok = true;
                result.actionsAlreadyExecuted = true;
                result.textResult = msg;
                if (logFn) logFn(L"  [诊断] " + TruncateForLog(msg, 160));
                return result;
            }
            AiCaptureMapping map;
            if (captureMapping) map = *captureMapping;
            map.apiWidth = captureWidth > 0 ? captureWidth : map.apiWidth;
            map.apiHeight = captureHeight > 0 ? captureHeight : map.apiHeight;
            ZoomRefineLocateOptions zopts;
            // 默认 1 级：紧凑粗框/归一化点可跳过（省 API）；过大框 lazy 补级自动升 2
            zopts.maxLevels = 1;
            zopts.adaptiveRefineDepth = true;
            const std::wstring locateTarget = ExtractClickTargetPhrase(resolvedPrompt);
            const ZoomRefineLocateResult zr = ExecuteZoomRefineLocate(
                core, locateTarget, screenshotBase64, captureWidth, captureHeight,
                map, stopFlag, logFn, httpAbort, zopts);
            if (!zr.ok) {
                result.ok = false;
                result.errorMessage = zr.errorMessage.empty()
                    ? L"定位失败" : zr.errorMessage;
                result.routeKind = route;
                return result;
            }
            if (logFn) {
                logFn(L"  [诊断] 定位完成：屏幕("
                    + std::to_wstring(zr.screenX) + L"," + std::to_wstring(zr.screenY)
                    + L") 识图轮次=" + std::to_wstring(zr.levelsUsed)
                    + (zr.skippedRefine ? L"（自适应跳过二级）" : L""));
            }
            int br = 0, bg = 0, bb = 0;
            const bool hadBefore = GetScreenPixelRgb(zr.screenX, zr.screenY, br, bg, bb);
            result.routeKind = route;
            const std::wstring clickButton = PromptIntendsRightClick(resolvedPrompt)
                ? L"right" : L"left";
            const int clickCount = PromptIntendsDoubleClick(resolvedPrompt) ? 2 : 1;
            result.textResult = BuildScreenClickActionsJson(
                zr.screenX, zr.screenY, true, clickButton, clickCount);
            result.ok = true;
            if (hadBefore && hostHooks && hostHooks->onExecuteActions) {
                hostHooks->onExecuteActions(result.textResult);
                result.actionsAlreadyExecuted = true;
                if (VerifyClickEffectByColorSample(zr.screenX, zr.screenY, br, bg, bb, 12)) {
                    if (logFn) logFn(L"  [诊断] 点击后采样点颜色已变化（可能生效）");
                } else if (logFn) {
                    logFn(L"  [诊断] 点击后采样点颜色接近（界面可能未变，仅供参考）");
                }
            }
            return result;
        }

        if (route == AiActionRouteKind::VisionQuery) {
            const std::wstring instruction = BuildAiActionExecuteUserInstruction(
                resolvedPrompt, captureWidth, captureHeight, withImage);
            const ChatMessage msg = BuildAiUserMessage(instruction, screenshotBase64,
                extraImageJpegBase64);
            result = RunVisionMessage(core, msg, contextMode, stopFlag, logFn, httpAbort);
            result.routeKind = route;
            if (result.ok && logFn) logFn(L"  [诊断] 识图问答完成");
            return result;
        }

        // Agent 闭环：里程碑执行 + 稀疏观察（对齐 Computer Use / Stagehand 选择性验收）
        if (agentic) {
            const bool plannerCanSeeImages = ModelSupportsVision(core->GetConfig().model);
            if (logFn && !plannerCanSeeImages) {
                logFn(L"  [诊断] 主模型「" + core->GetConfig().model
                    + L"」非多模态：观察截图不上传规划轮；"
                      L"locateAndClick 仍走识图子模型");
            }
            auto compactAgentHistory = [core, plannerCanSeeImages]() {
                if (!core) return;
                std::vector<ChatMessage> hist = core->GetHistory();
                size_t lastImgUser = hist.size();
                if (plannerCanSeeImages) {
                    for (size_t i = hist.size(); i > 0; --i) {
                        const auto& m = hist[i - 1];
                        if (m.role != L"user") continue;
                        for (const auto& p : m.parts) {
                            if (p.type == L"image_url") {
                                lastImgUser = i - 1;
                                break;
                            }
                        }
                        if (lastImgUser < hist.size()) break;
                    }
                }
                for (size_t i = 0; i < hist.size(); ++i) {
                    auto& m = hist[i];
                    // 纯文本规划模型：历史里所有图都剥掉（避免后续轮再次带上）
                    const bool stripThis = m.role == L"user" && !m.parts.empty()
                        && (!plannerCanSeeImages || i != lastImgUser);
                    if (stripThis) {
                        std::vector<ChatContentPart> kept;
                        bool stripped = false;
                        for (const auto& p : m.parts) {
                            if (p.type == L"image_url") {
                                stripped = true;
                                continue;
                            }
                            kept.push_back(p);
                        }
                        if (stripped) {
                            if (kept.empty()) {
                                ChatContentPart t;
                                t.type = L"text";
                                t.text = plannerCanSeeImages
                                    ? L"(历史截图已省略)"
                                    : L"(观察截图未上传：纯文本规划模型；点击用 locateAndClick)";
                                kept.push_back(t);
                            }
                            m.parts = std::move(kept);
                        }
                    }
                    if (m.role == L"tool" && m.content.size() > 720) {
                        m.content.resize(720);
                        m.content += L"\n...(历史工具结果已压缩)";
                    }
                }
                core->SetFullHistory(std::move(hist));
            };

            // maxAgentRounds<0：与 aiMaxSteps=-1 对齐，不因「轮次用尽」掐断（仅留安全上限）
            constexpr int kUnlimitedRoundSoftCap = 100;
            const bool unlimitedRounds = maxAgentRounds < 0;
            const int rounds = unlimitedRounds
                ? kUnlimitedRoundSoftCap
                : std::clamp(maxAgentRounds > 0 ? maxAgentRounds : 10, 1, 100);
            std::string curB64 = screenshotBase64;
            int curW = captureWidth;
            int curH = captureHeight;
            bool anyExecuted = false;
            bool completed = false;
            int consecutiveUnchangedRounds = 0;
            int consecutiveRepeatRounds = 0;
            int consecutiveBlindKeyRounds = 0;
            bool forceRefreshObserve = false;
            std::wstring blindKeyHint;
            std::wstring scrollNoopHint;
            bool toolNudgePending = false;
            bool toolNudgeUsed = false;
            bool lastObserveUnchanged = false;
            bool lastLocateFailed = false;
            bool lastOpenedWebpage = false;
            bool lastClickish = false;
            bool lastBatchWasScroll = false;
            int consecutiveScrollNoopRounds = 0;
            bool saveAsForeground = false;
            std::wstring lastSettleHint;
            std::wstring lastChangeRoisText;
            std::wstring repeatToolHint;
            std::wstring locateLoopHint;
            AiActionLookahead lookahead;
            std::wstring pendingLookaheadHint;
            std::wstring pendingPivotHint;

            auto historyHasLocateFail = [](const std::vector<ChatMessage>& hist, size_t afterIdx) {
                for (size_t i = afterIdx; i < hist.size(); ++i) {
                    const auto& m = hist[i];
                    if (m.role != L"tool") continue;
                    if (m.content.find(L"未找到目标") != std::wstring::npos
                        || m.content.find(L"定位失败") != std::wstring::npos
                        || m.content.find(L"NOT_FOUND") != std::wstring::npos) {
                        return true;
                    }
                }
                return false;
            };
            auto historyHasOpenWebpage = [](const std::vector<ChatMessage>& hist, size_t afterIdx) {
                for (size_t i = afterIdx; i < hist.size(); ++i) {
                    const auto& m = hist[i];
                    if (m.role != L"assistant") continue;
                    for (const auto& tc : m.tool_calls) {
                        if (tc.name == L"openWebpage") return true;
                    }
                }
                return false;
            };
            auto historyHasScrollWheel = [](const std::vector<ChatMessage>& hist, size_t afterIdx) {
                for (size_t i = afterIdx; i < hist.size(); ++i) {
                    const auto& m = hist[i];
                    if (m.role != L"assistant") continue;
                    for (const auto& tc : m.tool_calls) {
                        if (tc.name == L"scrollWheel") return true;
                    }
                }
                return false;
            };
            auto historyHasClickish = [](const std::vector<ChatMessage>& hist, size_t afterIdx) {
                for (size_t i = afterIdx; i < hist.size(); ++i) {
                    const auto& m = hist[i];
                    if (m.role != L"assistant") continue;
                    for (const auto& tc : m.tool_calls) {
                        if (tc.name == L"locateAndClick" || tc.name == L"mouseClick")
                            return true;
                    }
                }
                return false;
            };
            // 本批是否「只按键盘/快捷键、零视觉反馈」：keyClick/hotkeyShortcut/runActionRecipe
            auto historyIsBlindKeyOnly = [](const std::vector<ChatMessage>& hist, size_t afterIdx) {
                bool any = false;
                for (size_t i = afterIdx; i < hist.size(); ++i) {
                    const auto& m = hist[i];
                    if (m.role != L"assistant") continue;
                    for (const auto& tc : m.tool_calls) {
                        if (tc.name == L"keyClick" || tc.name == L"hotkeyShortcut"
                            || tc.name == L"runActionRecipe") {
                            any = true;
                            continue;
                        }
                        if (tc.name == L"wait") continue;
                        return false;
                    }
                }
                return any;
            };

            for (int round = 0; round < rounds; ++round) {
                if (stopFlag.load()) {
                    result.errorMessage = L"用户取消";
                    return result;
                }
                if (round == 0 && contextMode == 0) core->ClearHistory();

                const size_t histBefore = core->GetHistory().size();
                std::wstring instruction;
                // 规则放 Skill（lookupMacroAction）；此处只给短事实，避免塞长约束拖慢/卡流式
                if (toolNudgePending) {
                    toolNudgePending = false;
                    instruction =
                        L"上轮未调工具。立刻调用工具；细则 lookupMacroAction(section=usage|agent)。";
                } else if (round == 0) {
                    instruction = BuildAiActionExecuteUserInstruction(
                        resolvedPrompt, curW, curH, !curB64.empty());
                    if (!instruction.empty() && instruction.back() != L'\n')
                        instruction += L"\n";
                    instruction +=
                        L"用工具完成任务。同轮多工具；按观察自适应，勿套死剧本。"
                        L"已开应用 listWindows→activateWindow；启动 runProgram/openWebpage；"
                        L"路径用 resolveSystemPath。卡点时可 lookupMacroAction(section=agent)。";
                    // 任务点名 Edge/浏览器/历史时，首轮就钉死「必须开源」，防模型读空缓存后瞎填
                    {
                        const std::wstring p = resolvedPrompt;
                        const bool needBrowser =
                            p.find(L"Edge") != std::wstring::npos
                            || p.find(L"edge") != std::wstring::npos
                            || p.find(L"浏览器") != std::wstring::npos
                            || p.find(L"历史记录") != std::wstring::npos
                            || p.find(L"浏览记录") != std::wstring::npos;
                        if (needBrowser) {
                            instruction +=
                                L"\n★★本任务涉及浏览器/历史记录：必须先 openWebpage 或 "
                                L"openAppViaSearch(query=Edge或浏览器) 打开源并抄录→saveTaskData，"
                                L"禁止未打开浏览器就填写/编造历史条目，禁止假设桌面已有 xlsx。";
                        }
                    }
                } else if (lastLocateFailed) {
                    const int locateRetries = AiLocateRetryCount();
                    if (locateRetries >= 2) {
                        // A4 连败硬拦：只允许露出目标或结束，禁止继续换 target 碰运气
                        instruction = L"上轮定位失败，且已连续重试达到硬拦上限（"
                            + std::to_wstring(locateRetries)
                            + L" 次）。本轮禁止 locateAndClick/mouseClick 及非白名单快捷键。"
                              L"只允许 scrollWheel / activateWindow 让目标露出后重新观察，"
                              L"或直接 completeTask(reason=未找到)。细则 section=agent。";
                    } else {
                        instruction = L"上轮定位失败。换短标签再 locate 最多 1 次，或 completeTask。"
                            L"禁止 F12/应用专属快捷键/hotkeyShortcut 碰运气。细则 section=agent。";
                    }
                } else if (!lastSettleHint.empty()) {
                    // 宿主本地结论（短事实）；策略见 Skill
                    instruction = lastSettleHint;
                    if (!lastChangeRoisText.empty())
                        instruction += L" 结构变化区:" + lastChangeRoisText;
                    lastSettleHint.clear();
                } else if (lastOpenedWebpage) {
                    instruction = L"已 openWebpage。辨认是否可交互；加载中只 wait。"
                        L"细则 section=agent。";
                } else if (lastObserveUnchanged) {
                    if (consecutiveUnchangedRounds >= 1 || lastClickish) {
                        instruction =
                            L"界面未变。若有模态确认/错误弹窗：locateAndClick(确定/关闭/OK) "
                            L"或看图处理；禁止默认 Escape/Alt+F4（会关掉未完成的对话框）。"
                            L"禁止重复盲点同一坐标。清障后再继续任务。";
                    } else {
                        instruction = L"本地观察：界面无明显变化（未上传新图）。继续或 completeTask。";
                    }
                } else {
                    instruction = L"继续下一里程碑或 completeTask。有弹窗先看图处理，勿默认 Escape。";
                }
                if (!unlimitedRounds && round + 1 >= rounds)
                    instruction += L" 最后一轮：做完或 completeTask 说明卡点。";
                // 对话框：只注入探测事实，不教操作菜单元
                saveAsForeground = false;
                if (hostHooks && hostHooks->onProbeForegroundDialog) {
                    const std::wstring probe = hostHooks->onProbeForegroundDialog();
                    if (probe.find(L"kind=saveAs") != std::wstring::npos
                        || probe.find(L"kind=openFile") != std::wstring::npos) {
                        saveAsForeground = true;
                        if (!probe.empty())
                            instruction += L"\n[对话框] " + probe;
                        instruction +=
                            L"\n[事实] 另存为导航：优先文件名框 quickInput(完整路径或纯文件名, clearFirst)"
                            L"→Enter；禁止 scrollWheel 空转找侧栏。滚动条用 mouseDrag。";
                    }
                }
                if (!pendingLookaheadHint.empty()) {
                    instruction += L"\n" + pendingLookaheadHint;
                    pendingLookaheadHint.clear();
                }
                if (!pendingPivotHint.empty()) {
                    instruction += L"\n" + pendingPivotHint;
                    pendingPivotHint.clear();
                }
                if (!repeatToolHint.empty()) {
                    instruction += L"\n" + repeatToolHint;
                    repeatToolHint.clear();
                }
                if (!locateLoopHint.empty()) {
                    instruction += L"\n" + locateLoopHint;
                    locateLoopHint.clear();
                }
                if (!scrollNoopHint.empty()) {
                    instruction += L"\n" + scrollNoopHint;
                    scrollNoopHint.clear();
                }
                if (!blindKeyHint.empty()) {
                    instruction += L"\n" + blindKeyHint;
                    blindKeyHint.clear();
                }
                // 截图已带 CAPTUREBLT，能拍到输入法候选框/组字框；文本注入只在「正在组字」时给：
                // 候选框小字在观察帧里仍难读，且明确告知 Agent 按键正被 IME 拦截。
                if (hostHooks && hostHooks->onQueryImeStatus) {
                    const std::wstring ime = hostHooks->onQueryImeStatus();
                    if (!ime.empty() && ime.find(L"正在组字") != std::wstring::npos) {
                        instruction += L"\n" + ime;
                    }
                }
                if (round == 0 && AiActionPlanGateEnabled() && !AiActionPlanGateIsOpen()) {
                    instruction += L"\n★第一轮先规划再动手："
                        L"1) updateTaskMemo(section=goal|todos) 写出完整分步计划"
                        L"（每步：做什么→用哪个工具→怎么验收）；"
                        L"2) listWindows + activateWindow 确认目标窗口在前台。"
                        L"窗口未确认为前台前，禁止 locateAndClick/quickInput/mouseClick/keyClick/scrollWheel。";
                }
                {
                    const std::wstring memo = ReadAiTaskMemoText();
                    if (!memo.empty()) {
                        instruction += L"\n备忘:\n";
                        // 注入最多 6 行，省 token
                        int lines = 0;
                        size_t pos = 0;
                        while (pos < memo.size() && lines < 6) {
                            size_t nl = memo.find(L'\n', pos);
                            if (nl == std::wstring::npos) nl = memo.size();
                            instruction += memo.substr(pos, nl - pos);
                            instruction += L"\n";
                            pos = nl + (nl < memo.size() ? 1 : 0);
                            ++lines;
                        }
                    }
                }
                // ★数据缓存注入：识图过的数据源已存进本地缓存，直接用它，勿再回数据源重看
                {
                    const std::wstring data = ReadAiTaskDataSummary();
                    if (!data.empty()) {
                        instruction += L"\n★本轮已缓存数据（仅本轮 saveTaskData 写入；完整用 readTaskData）:\n"
                            + data + L"\n"
                            + L"若用户任务仍要求打开 Edge/浏览器获取「最新」历史，以任务为准先开源再抄，"
                              L"勿只靠缓存跳过。";
                    }
                }

                if (logFn) {
                    std::wstring roundLine = L"  [诊断] Agent 第 "
                        + std::to_wstring(round + 1) + L"/";
                    if (unlimitedRounds)
                        roundLine += L"不限(安全上限" + std::to_wstring(rounds) + L") 轮…";
                    else
                        roundLine += std::to_wstring(rounds) + L" 轮…";
                    logFn(roundLine);
                }

                // 首轮无图时工具曾禁止绝对坐标；观察回传截图后需放开，否则 mouseClick 被误拒
                // （纯文本规划模型虽不收图，宿主仍有截图映射，locateAndClick 可用）
                if (!curB64.empty()) {
                    AiActionToolOptions liveOpts;
                    liveOpts.allowAbsolutePointer = true;
                    liveOpts.fillTableOnly = fillTableOnly;
                    core->UpdateTools(BuildAiActionExecuteTools(hostHooks, liveOpts));
                }

                std::string attachB64 = curB64;
                if (!ShouldAttachObserveImageToPlanner(core->GetConfig().model, !attachB64.empty())) {
                    if (!attachB64.empty()) {
                        if (logFn) {
                            logFn(L"  [诊断] 本轮观察截图 "
                                + std::to_wstring(curW) + L"×" + std::to_wstring(curH)
                                + L" 已保留给 locate，不上传规划模型");
                        }
                        instruction +=
                            L"\n★你是纯文本规划模型：本轮未附截图（避免无效多模态请求）。"
                            L"宿主已完成本地 settle；要点击/辨认控件请 locateAndClick"
                            L"（自动用列表中的识图模型）。"
                            L"禁止猜绝对坐标；弹窗用 locate 点确定/关闭，勿默认 Escape。";
                        attachB64.clear();
                    }
                }

                if (round > 0) compactAgentHistory();
                const std::vector<std::string>* extrasNow = nullptr;
                if (round == 0 && extraImageJpegBase64 && !extraImageJpegBase64->empty()) {
                    // 文本规划模型会剥掉 attachB64；此时不要把剪贴板图再塞回去。
                    if (!attachB64.empty() || screenshotBase64.empty()) {
                        extrasNow = extraImageJpegBase64;
                    }
                }
                const ChatMessage msg = BuildAiUserMessage(instruction, attachB64, extrasNow);
                AgentSendCallbacks cb = MakeAiMacroSendCallbacks(logFn, &stopFlag, httpAbort);
                // 无图+工具 required 时部分网关流式首包极慢/空等；改完整响应更稳
                cb.preferNonStream = attachB64.empty();
                cb.toolChoice = L"required";
                cb.stopToolLoopAfterTools = [core, histBefore]() {
                    return HistoryHasCompleteTask(core->GetHistory(), histBefore)
                        || HistoryHasSubmitRequestingObserve(core->GetHistory(), histBefore)
                        || HistoryNeedsForceObserve(core->GetHistory(), histBefore);
                };

                const std::wstring response = core->SendMessage(msg, cb);
                if (stopFlag.load()) {
                    result.errorMessage = L"用户取消";
                    return result;
                }
                const bool roundApiError = IsAgentErrorResponse(response);
                if (roundApiError
                    && !HistoryHasSubmitMacroActions(core->GetHistory(), histBefore)
                    && !HistoryHasCompleteTask(core->GetHistory(), histBefore)) {
                    result.errorMessage = response;
                    if (logFn) logFn(L"  " + TruncateForLog(response));
                    return result;
                }

                lastLocateFailed = historyHasLocateFail(core->GetHistory(), histBefore);
                lastOpenedWebpage = historyHasOpenWebpage(core->GetHistory(), histBefore);
                lastClickish = historyHasClickish(core->GetHistory(), histBefore);
                lastBatchWasScroll = historyHasScrollWheel(core->GetHistory(), histBefore);
                // 连续定位失败：工具层已在第 3 次起硬拦截；这里提前一轮给模型方向
                if (lastLocateFailed && AiLocateRetryCount() >= 1) {
                    locateLoopHint = L"★已 " + std::to_wstring(AiLocateRetryCount())
                        + L" 次定位失败。停止换词重试 locateAndClick / scrollWheel："
                        L"画面没变不会变出目标。下一步：listWindows + activateWindow "
                        L"确认目标窗口确实在前台；或 completeTask(reason=未找到目标)。";
                } else {
                    locateLoopHint.clear();
                }
                const bool forceObserveSignals =
                    HistoryNeedsForceObserve(core->GetHistory(), histBefore) || roundApiError;
                if (forceObserveSignals) {
                    pendingPivotHint =
                        L"★上轮结果不确定（灰钮/无反应/定位失败/API超时）。"
                        L"必须先看新截图再规划；禁止盲 Enter、禁止切窗逃避未完成对话框。";
                    if (roundApiError)
                        pendingPivotHint += L" 上轮 API 失败，旧画面不可信。";
                }

                // 连续两轮调用完全相同的工具批次（含参数）→ 原地打转，下一轮注入劝阻
                {
                    std::wstring sig;
                    const auto& hist = core->GetHistory();
                    for (size_t i = histBefore; i < hist.size(); ++i) {
                        const auto& m = hist[i];
                        if (m.role != L"assistant") continue;
                        for (const auto& tc : m.tool_calls) {
                            sig += tc.name;
                            sig += L":";
                            if (tc.arguments.size() > 100)
                                sig += tc.arguments.substr(0, 100);
                            else
                                sig += tc.arguments;
                            sig += L"\n";
                        }
                    }
                    consecutiveRepeatRounds = AiNoteToolBatchSignature(sig);
                }
                // 只按键盘/快捷键且零视觉反馈的本批 → 供盲按键守卫统计
                const bool lastBatchBlindKeyOnly =
                    historyIsBlindKeyOnly(core->GetHistory(), histBefore);
                if (logFn && lastBatchBlindKeyOnly)
                    logFn(L"  [诊断] 本批仅键盘类动作（keyClick/hotkeyShortcut/配方）");
                if (consecutiveRepeatRounds >= 1) {
                    repeatToolHint =
                        L"★你连续两轮在做完全相同的动作（界面没变说明这条路不通）。"
                        L"停止原地重复：先看当前画面想清楚再动。"
                        L"若是输入错了：用 keyClick(Backspace) 或 keyClick(Delete) 删掉错误内容后重输，"
                        L"禁止乱按 Escape/菜单/快捷键碰运气。"
                        L"数据要用的内容：仅当本轮已 saveTaskData 时用 readTaskData；"
                        L"任务要求打开 Edge/浏览器时仍须先开源再抄。";
                }

                if (HistoryHasCompleteTask(core->GetHistory(), histBefore)) {
                    completed = true;
                    if (HistoryHasSubmitMacroActions(core->GetHistory(), histBefore))
                        anyExecuted = true;
                    result.completeReason = ExtractCompleteTaskReason(
                        core->GetHistory(), histBefore);
                    if (logFn) logFn(L"  [诊断] Agent 收到 completeTask，闭环结束");
                    break;
                }

                if (HistoryHasSubmitMacroActions(core->GetHistory(), histBefore)) {
                    anyExecuted = true;
                    const bool needObserve = HistoryHasSubmitRequestingObserve(
                        core->GetHistory(), histBefore);

                    // Midscene：定位失败 / 对话框前台 / 灰钮·无反应·API超时 → 禁止 SKIP_OBSERVE
                    const bool mustObserve = lastLocateFailed || saveAsForeground
                        || forceObserveSignals;
                    if (!needObserve && !mustObserve) {
                        if (logFn)
                            logFn(L"  [诊断] 本轮 SKIP_OBSERVE，跳过截屏观察（保留上次画面）");
                        if (!AiActionShouldSkipLookahead()) {
                            NoteAiActionLookaheadStarted();
                            const std::wstring batch = SummarizeAiToolBatchForLookahead(
                                core->GetHistory(), histBefore);
                            lookahead.BeginAfterTools(
                                core->GetConfig(), ReadAiTaskMemoText(), batch,
                                stopFlag, httpAbort);
                            pendingLookaheadHint = lookahead.TakeHintSkipObserve(4000);
                            if (logFn && !pendingLookaheadHint.empty())
                                logFn(L"  [诊断] lookahead(SKIP_OBSERVE): "
                                    + TruncateForLog(pendingLookaheadHint, 120));
                        } else if (logFn) {
                            logFn(L"  [诊断] 跳过 lookahead（动态干扰或次数上限）");
                        }
                        consecutiveUnchangedRounds = 0;
                        consecutiveBlindKeyRounds = 0;
                        lastObserveUnchanged = false;
                        continue;
                    }
                    if (lastLocateFailed) {
                        forceRefreshObserve = true;  // A3：失败后强制重截，禁用旧帧
                        if (logFn)
                            logFn(L"  [诊断] 上轮定位失败，本轮强制刷新观察（禁用旧画面）");
                    } else if (forceObserveSignals) {
                        forceRefreshObserve = true;
                        if (logFn) {
                            logFn(roundApiError
                                ? L"  [诊断] API 超时/失败后强制刷新观察（禁用旧画面）"
                                : L"  [诊断] 不确定结果（灰钮/无反应等）强制刷新观察");
                        }
                    } else if (saveAsForeground && logFn) {
                        logFn(L"  [诊断] 另存为在前台，禁止 SKIP_OBSERVE，强制刷新观察");
                    }

                    lastObserveUnchanged = false;
                    bool captured = false;
                    bool lastOnlyDynamicChanged = false;
                    if (!AiActionShouldSkipLookahead()) {
                        NoteAiActionLookaheadStarted();
                        const std::wstring batch = SummarizeAiToolBatchForLookahead(
                            core->GetHistory(), histBefore);
                        lookahead.BeginAfterTools(
                            core->GetConfig(), ReadAiTaskMemoText(), batch,
                            stopFlag, httpAbort);
                    } else if (logFn) {
                        logFn(L"  [诊断] 跳过 lookahead（动态干扰或次数上限）");
                    }
                    if (hostHooks->onObserveScreen) {
                        const bool forceRefresh = forceRefreshObserve;
                        forceRefreshObserve = false;
                        const AiObserveCaptureResult obs =
                            hostHooks->onObserveScreen(forceRefresh);
                        if (forceRefresh && logFn)
                            logFn(L"  [诊断] 盲按键守卫：强制刷新截图");
                        NoteAiActionUiBusy(obs.busyCoverageRatio,
                            obs.suggestRefresh && !obs.uiSettled);
                        // 游戏/广告页：与 locate busy 门闩对齐（10% / 动态仍在变 4%）
                        if (obs.busyCoverageRatio >= 0.10
                            || (obs.suggestRefresh && obs.busyCoverageRatio >= 0.04)) {
                            curB64.clear();
                            captured = true;
                            lastSettleHint =
                                L"前台动态干扰大（游戏/广告/视频）。禁止在页面内容上 locateAndClick。"
                                L"先开新标签页（浏览器通用键）再操作，或切到站外空白区；"
                                L"禁止反复刷新 / 反复开同一内容视图。";
                            if (logFn)
                                logFn(L"  [诊断] 高动态覆盖 "
                                    + std::to_wstring(static_cast<int>(obs.busyCoverageRatio * 100))
                                    + L"%，跳过上传整屏（省 token）");
                        }
                        if (!obs.settleHint.empty()) {
                            lastSettleHint = obs.settleHint;
                            if (!obs.changeRoisText.empty())
                                lastChangeRoisText = obs.changeRoisText;
                            if (logFn && (obs.settleChecked || obs.onlyDynamicChanged)) {
                                logFn(L"  [诊断] 本地 settle：" + TruncateForLog(obs.settleHint, 160)
                                    + (obs.suggestRefresh ? L" [建议刷新]" : L"")
                                    + (obs.onlyDynamicChanged ? L" [仅动态区]" : L""));
                            }
                        }
                        if (obs.ok && obs.unchanged) {
                            lastObserveUnchanged = true;
                            lastOnlyDynamicChanged = obs.onlyDynamicChanged;
                            ++consecutiveUnchangedRounds;
                            // 滚动无效守卫：连续滚轮但画面没变 → 滚轮作用到了别处，停止滚动
                            if (lastBatchWasScroll) {
                                ++consecutiveScrollNoopRounds;
                                if (consecutiveScrollNoopRounds >= 2) {
                                    scrollNoopHint =
                                        L"★已连续 " + std::to_wstring(consecutiveScrollNoopRounds)
                                        + L" 次滚动滚轮但画面没变——滚轮没有作用在当前目标窗口上"
                                        L"（鼠标可能不在目标窗口内）。停止滚动！"
                                        L"请确认鼠标在目标窗口内再滚，或用 mouseDrag 拖滚动条滑块；"
                                        L"若目标是输入框：直接 quickInput 输入即可，不需要滚动。";
                                    if (logFn)
                                        logFn(L"  [诊断] 滚动无效守卫：连续 "
                                            + std::to_wstring(consecutiveScrollNoopRounds)
                                            + L" 次滚轮画面未变，已注入警告");
                                }
                            } else {
                                consecutiveScrollNoopRounds = 0;
                            }
                            if (lastBatchBlindKeyOnly) ++consecutiveBlindKeyRounds;
                            else consecutiveBlindKeyRounds = 0;
                            if (consecutiveBlindKeyRounds >= 2) {
                                std::wstring imeStatus;
                                if (hostHooks && hostHooks->onQueryImeStatus) {
                                    imeStatus = hostHooks->onQueryImeStatus();
                                    if (imeStatus.empty()) imeStatus = L"[输入法] 无法读取";
                                }
                                blindKeyHint =
                                    L"★你已经连续 " + std::to_wstring(consecutiveBlindKeyRounds)
                                    + L" 轮只按键盘/快捷键但界面没变——你在盲按，必须停手。"
                                      L"本轮将强制刷新截图：先看清当前界面和光标位置再决定下一步。"
                                    + L"\n" + imeStatus + L"。"
                                    + L"若怀疑中文输入法把键吃掉了（出现拼音残留/按键被吞）："
                                      L"先 switchIme(mode=english)，"
                                      L"或点任务栏输入法图标切「英」（禁用 Ctrl+Space——表格软件里会全选整列）。"
                                      L"禁止继续连按 Escape/Delete/方向键碰运气；"
                                      L"改 locateAndClick 视觉点击目标单元格/控件。"
                                      L"目标已达成就直接 completeTask。";
                                forceRefreshObserve = true;
                                if (logFn)
                                    logFn(L"  [诊断] 盲按键守卫：连续 "
                                        + std::to_wstring(consecutiveBlindKeyRounds)
                                        + L" 轮只按键且界面未变，已注入警告");
                            }
                            curB64.clear();
                            captured = true;
                            if (logFn) {
                                std::wstring line = L"  [诊断] 本地观察：";
                                if (obs.onlyDynamicChanged) {
                                    line += L"控件区稳定（忽略视频/动画）";
                                } else {
                                    line += L"界面未变";
                                }
                                if (obs.changedRatio >= 0) {
                                    line += L"（结构差分 "
                                        + std::to_wstring(static_cast<int>(obs.changedRatio * 10000.0) / 100.0)
                                        + L"%";
                                    if (obs.rawChangedRatio >= 0) {
                                        line += L"/原始 "
                                            + std::to_wstring(static_cast<int>(
                                                obs.rawChangedRatio * 10000.0) / 100.0)
                                            + L"%";
                                    }
                                    if (obs.busyCoverageRatio >= 0) {
                                        line += L"/动态覆盖 "
                                            + std::to_wstring(static_cast<int>(
                                                obs.busyCoverageRatio * 10000.0) / 100.0)
                                            + L"%";
                                    }
                                    line += L"）";
                                } else if (obs.matchScore >= 0) {
                                    line += L"（匹配 "
                                        + std::to_wstring(static_cast<int>(obs.matchScore + 0.5))
                                        + L"%）";
                                }
                                line += L"，跳过上传";
                                if (lastClickish && !obs.onlyDynamicChanged)
                                    line += L"（悬停≠点击成功，请看内容是否进入下一状态）";
                                logFn(line);
                            }
                        } else if (obs.ok && !obs.base64.empty()) {
                            // 只有「已验证的真实变化」才重置防卡死守卫计数：
                            // 定位失败后的强制刷新会跳过差分（changedRatio=-1），
                            // 那种「已变」只是新基线，不算进展，否则守卫永远攒不到阈值，
                            // 模型可以无限「定位失败→刷新→再绕」。
                            const bool verifiedChange = obs.changedRatio >= 0.0005
                                || (obs.matchScore >= 0 && obs.matchScore < 99.9);
                            if (verifiedChange) {
                                consecutiveUnchangedRounds = 0;
                                consecutiveBlindKeyRounds = 0;
                                consecutiveScrollNoopRounds = 0;
                            }
                            curB64 = obs.base64;
                            curW = obs.width;
                            curH = obs.height;
                            captured = true;
                            if (logFn) {
                                std::wstring line = L"  [诊断] 本地观察：界面已变，已回传 "
                                    + std::to_wstring(curW) + L"×" + std::to_wstring(curH);
                                if (obs.changedRatio >= 0) {
                                    line += L"（差分 "
                                        + std::to_wstring(static_cast<int>(obs.changedRatio * 10000.0) / 100.0)
                                        + L"%）";
                                } else if (obs.matchScore >= 0) {
                                    line += L"（相对基线 "
                                        + std::to_wstring(static_cast<int>(obs.matchScore + 0.5))
                                        + L"%）";
                                }
                                if (!obs.changeRoisText.empty()) {
                                    line += L" 变化区[" + TruncateForLog(obs.changeRoisText, 80) + L"]";
                                    if (lastChangeRoisText.empty())
                                        lastChangeRoisText = obs.changeRoisText;
                                }
                                logFn(line);
                            }
                        } else if (logFn) {
                            logFn(L"  [诊断] 智能观察失败，尝试旧截屏接口…");
                        }
                    }
                    {
                        bool dialogBlocking = false;
                        if (hostHooks->onProbeForegroundDialog) {
                            const std::wstring probe = hostHooks->onProbeForegroundDialog();
                            dialogBlocking = !probe.empty()
                                && probe.find(L"kind=") != std::wstring::npos
                                && probe.find(L"kind=none") == std::wstring::npos;
                        }
                        pendingLookaheadHint = lookahead.TakeHintAfterObserve(
                            lastObserveUnchanged, lastOnlyDynamicChanged,
                            dialogBlocking, 6000);
                        if (logFn && !pendingLookaheadHint.empty())
                            logFn(L"  [诊断] lookahead: "
                                + TruncateForLog(pendingLookaheadHint, 120));
                        else if (logFn && lastObserveUnchanged)
                            logFn(L"  [诊断] lookahead 已丢弃（界面未变）");
                    }
                    if (!captured && hostHooks->onCaptureScreen) {
                        std::string nextB64;
                        int nw = 0, nh = 0;
                        if (hostHooks->onCaptureScreen(nextB64, nw, nh) && !nextB64.empty()) {
                            curB64 = std::move(nextB64);
                            curW = nw;
                            curH = nh;
                            if (logFn)
                                logFn(L"  [诊断] 已回传观察截图 " + std::to_wstring(curW) + L"×"
                                    + std::to_wstring(curH));
                        } else if (logFn) {
                            logFn(L"  [诊断] 观察截图失败，仍继续下一轮（无新图）");
                            curB64.clear();
                        }
                    } else if (!captured) {
                        curB64.clear();
                    }
                    continue;
                }

                AiActionResult fallback = FinalizeToolActionsResult(core, response, stopFlag, logFn);
                if (fallback.ok && !fallback.textResult.empty()) {
                    if (hostHooks->onExecuteActions) {
                        hostHooks->onExecuteActions(fallback.textResult);
                        anyExecuted = true;
                    }
                    result = fallback;
                    result.routeKind = route;
                    result.actionsAlreadyExecuted = true;
                    result.ok = true;
                    return result;
                }
                // 嘴炮纠偏：给一次机会改走 submit/completeTask，避免只在调试框吐正文
                if (!toolNudgeUsed && round + 1 < rounds) {
                    toolNudgeUsed = true;
                    toolNudgePending = true;
                    if (logFn)
                        logFn(L"  [诊断] 未调用工具（疑似嘴炮），注入纠正提示再试一轮");
                    continue;
                }
                result.errorMessage = fallback.errorMessage.empty()
                    ? L"Agent 未调用动作工具/completeTask" : fallback.errorMessage;
                result.textResult = fallback.textResult;
                return result;
            }

            result.routeKind = route;
            result.actionsAlreadyExecuted = true;
            result.anyActionsExecuted = anyExecuted;
            if (completed) {
                // 逻辑转化会话：失败措辞的 completeTask 不当成成功（验收对齐 OpenAdapt halt）。
                // 普通（非逻辑转化）AI 动作执行保持既有语义：只要 AI 验收就按成功计，
                // 避免把「未找到目标但仍返回」等正常收尾误判为 API 失败。
                if (AiLogicConvertSessionActive()
                    && AiLogicConvertLooksLikeFailedComplete(result.completeReason)) {
                    result.ok = false;
                    result.textResult = L"[agent-complete-failed]";
                    result.errorMessage = result.completeReason.empty()
                        ? L"Agent 以失败原因 completeTask" : result.completeReason;
                    if (logFn)
                        logFn(L"  [诊断] completeTask 含失败措辞 → 视为未完成");
                } else {
                    result.ok = true;
                    result.textResult = result.completeReason.empty()
                        ? L"[agent-complete]"
                        : (L"[agent-complete] " + result.completeReason);
                }
            } else if (anyExecuted) {
                result.ok = false;
                result.textResult = L"[agent-partial]";
                result.errorMessage = unlimitedRounds
                    ? L"Agent 已达安全轮次上限仍未验收完成（请检查任务是否卡住）"
                    : L"Agent 轮次用尽，任务未验收完成（仅完成部分步骤）";
                if (logFn)
                    logFn(L"  [诊断] Agent 未 completeTask 即结束 → 视为未完成");
            } else {
                result.ok = false;
                result.errorMessage = L"Agent 闭环未提交动作也未完成";
            }
            return result;
        }

        // 无宿主钩子：旧路径（收集动作 JSON 交给调用方执行）
        if (route == AiActionRouteKind::MultiTurnTools) {
            static const wchar_t* kFollowUp =
                L"请继续：用 quickInput/keyClick/mouseClick 等规范工具，或 submitMacroActions；"
                L"验收用 completeTask。分步见 lookupMacroAction(section=agent)。";
            std::wstring mergedActions;
            for (int round = 0; round < 3; ++round) {
                if (stopFlag.load()) break;
                if (round == 0 && contextMode == 0) core->ClearHistory();
                const size_t histBefore = core->GetHistory().size();
                const std::wstring instruction = (round == 0)
                    ? BuildAiActionExecuteUserInstruction(
                        resolvedPrompt, captureWidth, captureHeight, withImage)
                    : std::wstring(kFollowUp);
                const ChatMessage msg = BuildAiUserMessage(
                    instruction, round == 0 ? screenshotBase64 : std::string{},
                    round == 0 ? extraImageJpegBase64 : nullptr);
                AgentSendCallbacks cb = MakeAiMacroSendCallbacks(logFn, &stopFlag, httpAbort);
                cb.preferNonStream = false;
                cb.toolChoice = L"required";
                cb.stopToolLoopAfterTools = [core, histBefore]() {
                    return HistoryHasCompleteTask(core->GetHistory(), histBefore)
                        || HistoryHasSubmitMacroActions(core->GetHistory(), histBefore);
                };
                if (logFn && round > 0)
                    logFn(L"  [诊断] 多轮工具 第 " + std::to_wstring(round + 1) + L" 轮…");
                const std::wstring response = core->SendMessage(msg, cb);
                if (HistoryHasCompleteTask(core->GetHistory(), histBefore)) {
                    result.routeKind = route;
                    result.textResult = mergedActions.empty() ? L"[]" : mergedActions;
                    result.ok = true;
                    return result;
                }
                AiActionResult roundResult = FinalizeToolActionsResult(core, response, stopFlag, logFn);
                if (!roundResult.textResult.empty() && roundResult.ok) {
                    mergedActions = roundResult.textResult;
                    if (JsonArrayHasStopMacro(mergedActions)) break;
                    if (round == 2) break;
                    continue;
                }
                if (round == 0) return roundResult;
                break;
            }
            result.routeKind = route;
            result.textResult = mergedActions;
            result.ok = !mergedActions.empty();
            if (!result.ok) result.errorMessage = L"复杂组合任务未完成（未提交有效动作）";
            return result;
        }

        // ToolExecute（无钩子）
        if (contextMode == 0) core->ClearHistory();
        const size_t histBefore = core->GetHistory().size();
        const std::wstring instruction = BuildAiActionExecuteUserInstruction(
            resolvedPrompt, captureWidth, captureHeight, withImage);
        const ChatMessage msg = BuildAiUserMessage(instruction, screenshotBase64,
            extraImageJpegBase64);
        AgentSendCallbacks cb = MakeAiMacroSendCallbacks(logFn, &stopFlag, httpAbort);
        cb.preferNonStream = false;
        cb.toolChoice = L"required";
        cb.stopToolLoopAfterTools = [core, histBefore]() {
            return HistoryHasCompleteTask(core->GetHistory(), histBefore)
                || HistoryHasSubmitMacroActions(core->GetHistory(), histBefore);
        };
        const std::wstring response = core->SendMessage(msg, cb);
        if (HistoryHasCompleteTask(core->GetHistory(), histBefore)
            && ExtractSubmittedActionsJson(core).empty()) {
            result.routeKind = route;
            result.textResult = L"[]";
            result.ok = true;
            return result;
        }
        result = FinalizeToolActionsResult(core, response, stopFlag, logFn);
        result.routeKind = route;
        return result;

    } catch (const std::exception& e) {
        result.errorMessage = L"API 调用异常：" + FromUtf8(e.what());
    } catch (...) {
        result.errorMessage = L"API 调用异常";
    }
    (void)timeoutSec;
    return result;
}
