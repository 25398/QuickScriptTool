#include "ai_action_service.h"

#include "ai_action_router.h"
#include "macro_execute_tools.h"
#include "script_action_builder.h"

#include "utils.h"



#include <opencv2/opencv.hpp>



#include <algorithm>

#include <chrono>
#include <memory>
#include <vector>



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



cv::Mat MatFromBitmap(HBITMAP hbmp) {

    if (!hbmp) return {};

    BITMAP bm{};

    if (!GetObject(hbmp, sizeof(bm), &bm) || bm.bmWidth <= 0 || bm.bmHeight <= 0) return {};



    BITMAPINFO bmi{};

    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);

    bmi.bmiHeader.biWidth = bm.bmWidth;

    bmi.bmiHeader.biHeight = -bm.bmHeight;

    bmi.bmiHeader.biPlanes = 1;

    bmi.bmiHeader.biBitCount = 24;

    bmi.bmiHeader.biCompression = BI_RGB;



    cv::Mat mat(bm.bmHeight, bm.bmWidth, CV_8UC3);

    HDC hdc = GetDC(nullptr);

    if (!hdc) return {};

    GetDIBits(hdc, hbmp, 0, bm.bmHeight, mat.data, &bmi, DIB_RGB_COLORS);

    ReleaseDC(nullptr, hdc);

    return mat;

}



std::wstring TruncateForLog(const std::wstring& text, size_t maxLen = 240) {

    if (text.size() <= maxLen) return text;

    return text.substr(0, maxLen) + L"...";

}



std::wstring ExtractJsonArrayFromText(const std::wstring& text) {

    const size_t start = text.find(L'[');

    const size_t end = text.rfind(L']');

    if (start == std::wstring::npos || end == std::wstring::npos || end <= start) return L"";

    return text.substr(start, end - start + 1);

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

    cb.onContentDelta = [logFn, contentStarted](const std::wstring& delta) {

        if (delta.empty()) return;

        if (!*contentStarted) {

            *contentStarted = true;

            logFn(L"  生成回复中…");

        }

        logFn(L"  回复：" + TruncateForLog(delta, 120));

    };

    cb.onToolCall = [logFn](const std::wstring& name, const std::wstring& args) {

        if (logFn) {
            std::wstring preview = args;
            if (preview.size() > 180) preview = preview.substr(0, 180) + L"...";
            logFn(L"  调用工具：" + name + L" 参数=" + preview);
        }

    };

    cb.onToolResult = [logFn](const std::wstring& name, const std::wstring& result) {

        if (!logFn || !IsMacroExecutionToolName(name)) return;

        if (result.rfind(L"[错误]", 0) == 0) {
            logFn(L"  工具返回错误：" + TruncateForLog(result, 160));
            return;
        }

        const std::wstring arr = ExtractJsonArrayFromText(result);

        if (arr.empty()) {
            logFn(L"  工具返回：非动作 JSON（可能是 lookupMacroAction 说明）");
            return;
        }

        try {
            const nlohmann::json j = nlohmann::json::parse(ToUtf8(arr));
            if (j.is_array()) {
                logFn(L"  工具返回动作 " + std::to_wstring(j.size()) + L" 个");
                for (size_t i = 0; i < j.size() && i < 5; ++i) {
                    if (j[i].is_object() && j[i].contains("type")) {
                        logFn(L"    · " + FromUtf8(j[i]["type"].get<std::string>()));
                    }
                }
                if (j.size() > 5) logFn(L"    · ...");
            }
        } catch (...) {
            logFn(L"  工具返回：JSON 解析失败");
        }

    };

    return cb;

}



std::string BitmapToBase64Jpeg(HBITMAP hBitmap, int quality, double scale) {

    if (!hBitmap) return {};

    cv::Mat mat = MatFromBitmap(hBitmap);

    if (mat.empty()) return {};

    scale = std::clamp(scale, 0.1, 1.0);

    if (scale < 0.999) {
        cv::resize(mat, mat, cv::Size(), scale, scale, cv::INTER_AREA);
    }

    std::vector<uint8_t> encoded;

    std::vector<int> params;

    params.push_back(cv::IMWRITE_JPEG_QUALITY);

    params.push_back(std::clamp(quality, 10, 100));

    if (!cv::imencode(".jpg", mat, encoded, params)) return {};

    return Base64Encode(encoded);

}

double ComputeEffectiveAiImageScale(int width, int height, double userScale) {
    userScale = std::clamp(userScale, 0.1, 1.0);
    if (width <= 0 || height <= 0) return userScale;
    constexpr int kMaxLongEdge = 1280;
    const double longAfterUser = static_cast<double>(std::max(width, height)) * userScale;
    if (longAfterUser <= kMaxLongEdge) return userScale;
    return userScale * static_cast<double>(kMaxLongEdge) / longAfterUser;
}

AiImageEncodeResult EncodeBitmapForAiAnalysis(HBITMAP hBitmap, double userScale) {
    AiImageEncodeResult out;
    if (!hBitmap) return out;
    BITMAP bm{};
    if (!GetObject(hBitmap, sizeof(bm), &bm) || bm.bmWidth <= 0 || bm.bmHeight <= 0) return out;
    out.srcWidth = bm.bmWidth;
    out.srcHeight = bm.bmHeight;
    out.effectiveScale = ComputeEffectiveAiImageScale(bm.bmWidth, bm.bmHeight, userScale);
    out.outWidth = std::max(1, static_cast<int>(bm.bmWidth * out.effectiveScale));
    out.outHeight = std::max(1, static_cast<int>(bm.bmHeight * out.effectiveScale));
    out.base64 = BitmapToBase64Jpeg(hBitmap, 72, out.effectiveScale);
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



    return std::make_unique<AgentCore>(cfg, extraSystemPrompt, std::vector<AgentTool>{});

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

    std::wstring prompt =
        L"你是 Windows 桌面宏动作执行器。\n"
        L"★ 必须通过 submitMacroActions 提交动作；不确定参数字段时先 lookupMacroAction。\n"
        L"★ 禁止在文字中描述、模拟或声称已执行操作；文字回复留空即可。\n"
        L"★ 禁止 markdown、emoji。\n\n"
        + ScriptActionCatalog() + L"\n";

    if (withImage && captureWidth > 0 && captureHeight > 0) {
        prompt += L"截图尺寸 " + std::to_wstring(captureWidth) + L"×" + std::to_wstring(captureHeight)
            + L"，坐标相对截图左上角；右下角约为 (" + std::to_wstring(captureWidth - 1) + L","
            + std::to_wstring(captureHeight - 1) + L")。\n";
    }

    prompt += L"流程：熟悉 type → 直接 submitMacroActions；不熟 → lookupMacroAction(type) → submitMacroActions。\n";

    return prompt;

}



}  // namespace



AiActionResult ExecuteAiTextAnalysis(

    AgentCore* core,

    const std::wstring& resolvedPrompt,

    int /*outputType*/,

    const std::atomic_bool& stopFlag,

    int timeoutSec,

    AiMacroLogFn logFn,

    AiHttpAbortSlot* httpAbort) {

    AiActionResult result;

    if (!core) {

        result.errorMessage = L"AgentCore 未创建";

        return result;

    }

    if (stopFlag.load()) {

        result.errorMessage = L"用户取消";

        return result;

    }



    ChatMessage msg;

    msg.role = L"user";

    msg.content = resolvedPrompt;



    AgentSendCallbacks cb = MakeAiMacroSendCallbacks(logFn, &stopFlag, httpAbort);
    cb.preferNonStream = true;
    if (core) core->SetRecvTimeoutMs(ResolveAiActionExecuteTimeoutSec(timeoutSec, false) * 1000);

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

    AiHttpAbortSlot* httpAbort) {

    AiActionResult result;

    if (!core) {

        result.errorMessage = L"AgentCore 未创建";

        return result;

    }

    if (stopFlag.load()) {

        result.errorMessage = L"用户取消";

        return result;

    }



    ChatMessage msg;

    msg.role = L"user";

    if (!resolvedPrompt.empty()) {

        ChatContentPart textPart;

        textPart.type = L"text";

        textPart.text = resolvedPrompt;

        msg.parts.push_back(textPart);

    }

    ChatContentPart imgPart;

    imgPart.type = L"image_url";

    imgPart.image_url = L"data:image/jpeg;base64," + FromUtf8(screenshotBase64);

    msg.parts.push_back(imgPart);



    const int effectiveSec = ResolveAiImageAnalysisTimeoutSec(timeoutSec, resolvedPrompt.size());
    if (core) core->SetRecvTimeoutMs(effectiveSec * 1000);

    AgentSendCallbacks cb = MakeAiMacroSendCallbacks(logFn, &stopFlag, httpAbort);
    cb.preferNonStream = false;

    if (logFn) {
        logFn(L"  超时上限 " + std::to_wstring(effectiveSec) + L"s，流式接收回复…");
        if (resolvedPrompt.size() > 400)
            logFn(L"  prompt 较长（" + std::to_wstring(resolvedPrompt.size())
                + L" 字），思考阶段可能较久");
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

int ResolveAiImageAnalysisTimeoutSec(int userTimeoutSec, size_t promptChars) {
    const int base = userTimeoutSec > 0 ? userTimeoutSec : 90;
    const int extra = static_cast<int>(std::min<size_t>(promptChars / 60, 180));
    return std::max(base + extra, 90);
}



namespace {



ChatMessage BuildAiUserMessage(const std::wstring& text, const std::string& screenshotBase64) {

    ChatMessage msg;

    msg.role = L"user";

    if (!screenshotBase64.empty()) {

        if (!text.empty()) {

            ChatContentPart textPart;

            textPart.type = L"text";

            textPart.text = text;

            msg.parts.push_back(textPart);

        }

        ChatContentPart imgPart;

        imgPart.type = L"image_url";

        imgPart.image_url = L"data:image/jpeg;base64," + FromUtf8(screenshotBase64);

        msg.parts.push_back(imgPart);

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

    cb.preferNonStream = false;

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

    try {

        const nlohmann::json merged = nlohmann::json::parse(ToUtf8(actionsJson));

        if (merged.is_array()) {

            logFn(L"  [诊断] 解析到 " + std::to_wstring(merged.size()) + L" 个动作");

        }

    } catch (...) {

        logFn(L"  [诊断] 动作 JSON 解析失败");

    }

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

    const AiCaptureMapping* captureMapping) {

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



    if (logFn) {

        logFn(L"  [诊断] 路由: " + AiActionRouteLabel(route));

        logFn(L"  [诊断] 用户消息含图片: " + std::wstring(withImage ? L"是" : L"否"));

        if (withImage)

            logFn(L"  [诊断] base64字节数: " + std::to_wstring(screenshotBase64.size()));

        logFn(L"  [诊断] API 超时: " + std::to_wstring(effectiveTimeoutSec) + L"s");

        logFn(L"  [诊断] 上下文模式: " + std::to_wstring(contextMode));

        logFn(L"  [诊断] 传输: " + std::wstring(withImage ? L"流式" : L"完整响应"));

    }



    try {

        if (route == AiActionRouteKind::CompositeClick) {

            const std::wstring locatePrompt = BuildCompositeLocatePrompt(resolvedPrompt);

            const ChatMessage msg = BuildAiUserMessage(locatePrompt, screenshotBase64);

            AiActionResult vision = RunVisionMessage(

                core, msg, contextMode, stopFlag, logFn, httpAbort);

            if (!vision.ok) return vision;

            int apiX = 0, apiY = 0;

            if (!TryParseCoordinatePair(vision.textResult, apiX, apiY)) {

                vision.ok = false;

                vision.errorMessage = L"无法从识图结果解析坐标：" + TruncateForLog(vision.textResult, 80);

                return vision;

            }

            AiCaptureMapping map;

            if (captureMapping) map = *captureMapping;

            map.apiWidth = captureWidth > 0 ? captureWidth : map.apiWidth;

            map.apiHeight = captureHeight > 0 ? captureHeight : map.apiHeight;

            int screenX = apiX, screenY = apiY;

            if (map.capX2 > map.capX1 && map.capY2 > map.capY1)

                MapApiPointToScreen(map, apiX, apiY, screenX, screenY);

            if (logFn) {

                logFn(L"  [诊断] 组合点击：api(" + std::to_wstring(apiX) + L"," + std::to_wstring(apiY)

                    + L") → 屏幕(" + std::to_wstring(screenX) + L"," + std::to_wstring(screenY) + L")");

            }

            result.routeKind = route;

            result.textResult = BuildScreenClickActionsJson(screenX, screenY, true);

            result.ok = true;

            return result;

        }



        if (route == AiActionRouteKind::VisionQuery) {

            const std::wstring instruction = BuildAiActionExecuteUserInstruction(

                resolvedPrompt, captureWidth, captureHeight, withImage);

            const ChatMessage msg = BuildAiUserMessage(instruction, screenshotBase64);

            result = RunVisionMessage(core, msg, contextMode, stopFlag, logFn, httpAbort);

            result.routeKind = route;

            if (result.ok && logFn) logFn(L"  [诊断] 识图问答完成");

            return result;

        }



        if (route == AiActionRouteKind::MultiTurnTools) {

            static const wchar_t* kFollowUp =

                L"请继续：若已定位目标，submitMacroActions 提交 moveMouse、mouseClick 等后续动作；"

                L"全部完成后须含 stopMacro。不确定参数可 lookupMacroAction(section=composite)。";

            std::wstring mergedActions;

            for (int round = 0; round < 3; ++round) {

                if (stopFlag.load()) break;

                if (round == 0 && contextMode == 0) core->ClearHistory();

                const std::wstring instruction = (round == 0)

                    ? BuildAiActionExecuteUserInstruction(

                        resolvedPrompt, captureWidth, captureHeight, withImage)

                    : std::wstring(kFollowUp);

                const ChatMessage msg = BuildAiUserMessage(instruction, round == 0 ? screenshotBase64 : std::string{});

                AgentSendCallbacks cb = MakeAiMacroSendCallbacks(logFn, &stopFlag, httpAbort);

                cb.preferNonStream = !withImage;

                cb.stopToolLoopAfterTools = [core]() {

                    return !ExtractSubmittedActionsJson(core).empty();

                };

                if (logFn && round > 0) logFn(L"  [诊断] 多轮工具 第 " + std::to_wstring(round + 1) + L" 轮…");

                const std::wstring response = core->SendMessage(msg, cb);

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



        // ToolExecute

        if (contextMode == 0) core->ClearHistory();

        const std::wstring instruction = BuildAiActionExecuteUserInstruction(

            resolvedPrompt, captureWidth, captureHeight, withImage);

        const ChatMessage msg = BuildAiUserMessage(instruction, screenshotBase64);

        AgentSendCallbacks cb = MakeAiMacroSendCallbacks(logFn, &stopFlag, httpAbort);

        cb.preferNonStream = !withImage;

        cb.stopToolLoopAfterTools = [core]() {

            return !ExtractSubmittedActionsJson(core).empty();

        };

        const std::wstring response = core->SendMessage(msg, cb);

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
