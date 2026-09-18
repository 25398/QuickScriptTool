#include "ai_logic_convert.h"

#include "action_tree.h"
#include "action_utils.h"
#include "image_match.h"
#include "opencv_runtime.h"
#include "image_var_util.h"
#include "recording_to_findimage.h"
#include "script_io.h"
#include "utils.h"

#include <opencv2/opencv.hpp>

#include <windows.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cwctype>
#include <functional>
#include <vector>

bool LooksLikeListOcrText(const std::wstring& text) {
    const std::wstring t = Trim(text);
    if (t.size() < 8) return false;
    // 错屏/对话框先拒（勿被多行另存为 UI 误判成列表）
    if (t.find(L"另存为") != std::wstring::npos || t.find(L"文件名") != std::wstring::npos
        || t.find(L"费用中心") != std::wstring::npos || t.find(L"Save As") != std::wstring::npos)
        return false;
    int newlines = 0;
    for (wchar_t c : t) if (c == L'\n' || c == L'\r') ++newlines;
    if (newlines >= 2) return true;
    // 时间 / URL / 常见列表痕迹（通用启发，非站点词典）
    if (t.find(L"http") != std::wstring::npos || t.find(L"www.") != std::wstring::npos)
        return true;
    if (t.find(L":") != std::wstring::npos) {
        // 粗判 HH:MM
        for (size_t i = 0; i + 4 < t.size(); ++i) {
            if (t[i] >= L'0' && t[i] <= L'9' && t[i + 1] >= L'0' && t[i + 1] <= L'9'
                && t[i + 2] == L':' && t[i + 3] >= L'0' && t[i + 3] <= L'9')
                return true;
        }
    }
    // 较长单段也可能是列表粘贴
    return t.size() >= 40 && newlines >= 1;
}

double EstimateFindImageTemplateFeatureScore(const std::wstring& imagePath) {
    if (imagePath.empty() || !OpenCvAvailable()) return -1.0;
    try {
        const cv::Mat img = cv::imread(ToUtf8(imagePath), cv::IMREAD_GRAYSCALE);
        if (img.empty() || img.cols < 4 || img.rows < 4) return -1.0;
        cv::Scalar mean, stddev;
        cv::meanStdDev(img, mean, stddev);
        return stddev[0];
    } catch (...) {
        return -1.0;
    }
}

namespace {

struct LogicSession {
    bool active = false;
    bool healPromote = false;
    bool pendingWriteback = false;
    std::wstring blockName;
    std::wstring prompt;
    std::wstring scriptPath;
    int sourceOriginalNo = 0;
    std::vector<AiLogicTranscriptEntry> transcript;
};

LogicSession& Sess() {
    thread_local LogicSession s;
    return s;
}

bool IsConvertibleType(ActionType t) {
    switch (t) {
    case ActionType::KeyClick:
    case ActionType::KeyDown:
    case ActionType::KeyUp:
    case ActionType::HotkeyShortcut:
    case ActionType::QuickInput:
    case ActionType::Wait:
    case ActionType::OpenWebpage:
    case ActionType::OpenFile:
    case ActionType::RunProgram:
    case ActionType::ActivateWindow:
    case ActionType::ScrollWheel:
        return true;
    default:
        return false;
    }
}

ScriptAction MakeIndent(ScriptAction a, int indent) {
    a.indent = indent;
    return a;
}

ScriptAction MakeDefine(const std::wstring& name) {
    ScriptAction a;
    a.type = ActionType::DefineBlock;
    a.blockName = name;
    a.indent = 0;
    a.remark = L"逻辑转化自动生成";
    return a;
}

ScriptAction MakeRun(const std::wstring& name) {
    ScriptAction a;
    a.type = ActionType::RunBlock;
    a.blockName = name;
    a.indent = 0;
    a.clickCount = 1;
    a.duration = 0.01;
    a.randomDuration = 0.0;
    a.remark = L"逻辑转化入口";
    return a;
}

ScriptAction MakeTimer(const std::wstring& varName, int indent) {
    ScriptAction a;
    a.type = ActionType::TimerRecordTime;
    a.loopVarName = varName;
    a.indent = indent;
    a.remark = L"逻辑转化计时";
    return a;
}

ScriptAction MakeFindImageClick(const std::wstring& imagePath, int indent,
    const std::wstring& button, int clickCount, const std::wstring& remark,
    const std::wstring& findTimeExpr, int anchorX = 0, int anchorY = 0) {
    ScriptAction a;
    a.type = ActionType::FindImage;
    a.imagePath = imagePath;
    a.searchFullScreen = true;
    a.matchThreshold = 85.0;
    a.findImageFollowUp = 0; // 点击
    a.findTimeExpr = findTimeExpr.empty() ? L"15" : findTimeExpr;
    a.imageScaleMin = 0.9;
    a.imageScaleMax = 1.1;
    a.imageScale = 1.0;
    a.button = (button == L"right") ? MouseButtonType::Right : MouseButtonType::Left;
    a.clickCount = std::max(1, clickCount);
    a.indent = indent;
    a.remark = remark;
    // 低纹理模板：抬阈值 + 锚点邻域搜索，减少假 100% 乱点
    const double feat = EstimateFindImageTemplateFeatureScore(imagePath);
    if (feat >= 0.0 && feat < 5.0) {
        a.matchThreshold = 98.0;
        a.remark += L"（极低特征·从严）";
    } else if (feat >= 0.0 && feat < 12.0) {
        a.matchThreshold = 92.0;
        a.remark += L"（低特征门闩）";
    }
    if (anchorX != 0 && anchorY != 0 && feat >= 0.0 && feat < 12.0) {
        const int pad = 280;
        a.searchFullScreen = false;
        a.searchX1 = anchorX - pad;
        a.searchY1 = anchorY - pad;
        a.searchX2 = anchorX + pad;
        a.searchY2 = anchorY + pad;
    }
    return a;
}

ScriptAction MakeGapWait(int indent, double sec) {
    ScriptAction a;
    a.type = ActionType::Wait;
    a.duration = sec < 0.01 ? 0.01 : sec;
    if (a.duration > 2.0) a.duration = 2.0;
    a.indent = indent;
    a.remark = L"步间间隔";
    return a;
}

/// 找图限时：参考 settle，夹在 [8,30]；无 settle 默认 15
int LocateFindTimeSec(const AiLogicTranscriptEntry& e) {
    int t = 15;
    if (e.settleSec > 0.05) {
        t = static_cast<int>(std::ceil(e.settleSec * 4.0));
        if (t < 8) t = 8;
        if (t > 30) t = 30;
    }
    return t;
}

double GapWaitSecFor(const AiLogicTranscriptEntry& e) {
    if (e.fromLocate || e.fromActivate) {
        if (e.settleSec > 0.05) {
            double s = e.settleSec * 0.15;
            if (s < 0.05) s = 0.05;
            if (s > 0.8) s = 0.8;
            return s;
        }
        return 0.08;
    }
    if (e.settleSec > 0.05) {
        double s = e.settleSec * 0.05;
        if (s < 0.01) s = 0.01;
        if (s > 0.3) s = 0.3;
        return s;
    }
    return 0.05;
}

ScriptAction MakeIf(const std::wstring& expr, int indent) {
    ScriptAction a;
    a.type = ActionType::If;
    a.conditionExpr = expr;
    a.indent = indent;
    return a;
}

ScriptAction MakeElse(int indent) {
    ScriptAction a;
    a.type = ActionType::Else;
    a.indent = indent;
    return a;
}

ScriptAction MakeFallbackAi(const std::wstring& prompt, const std::wstring& blockName, int indent) {
    ScriptAction a;
    a.type = ActionType::AiActionExecute;
    a.aiPrompt = prompt;
    // 修界面漂移必须先看到当前画面；否则首轮无图，回退 AI 只能盲试
    a.aiWithImage = true;
    a.aiMaxSteps = -1;
    // 仍标逻辑转化：回退成功可 promote；会话侧用 remark 识别 heal，避免整段首次写回
    a.aiLogicConvert = true;
    a.aiLogicBlockName = blockName;
    a.aiContextMode = 1;
    a.indent = indent;
    a.remark = L"逻辑转化回退（界面漂移时自愈并提升快路径）";
    return a;
}

ScriptAction MakeActivateSettle(const std::wstring& match, int indent) {
    ScriptAction a;
    a.type = ActionType::ActivateWindow;
    a.targetPath = Trim(match);
    a.indent = indent;
    // 可回放宏动作：执行器按标题/进程子串激活（比裸 Alt+Tab 能对准具名窗口）。
    // 备注保留 activateWindow: 供折叠/摘要识别；勿写 customText 抢列表显示名。
    a.remark = L"activateWindow:" + a.targetPath;
    a.customText.clear();
    return a;
}

std::wstring CanonicalLocateRemark(const std::wstring& target, bool isGate) {
    std::wstring t = Trim(target);
    for (auto& c : t) {
        if (c == L'\r' || c == L'\n' || c == L'\t') c = L' ';
    }
    while (t.find(L"  ") != std::wstring::npos)
        t.replace(t.find(L"  "), 2, L" ");
    if (t.size() > 18) t = t.substr(0, 18) + L"…";
    if (t.empty()) return isGate ? L"找图门闩" : L"找图点击";
    return (isGate ? L"找图门闩:" : L"找图点击:") + t;
}

bool LooksLikeStaticHeaderText(const std::wstring& s) {
    const std::wstring t = Trim(s);
    if (t.empty()) return false;
    static const wchar_t* kHeaders[] = {
        L"序号", L"标题", L"时间", L"名称", L"地址", L"网址", L"链接",
        L"日期", L"备注", L"内容", L"编号", L"索引", L"Name", L"Title", L"Time", L"URL",
    };
    for (const wchar_t* h : kHeaders) {
        if (t == h) return true;
    }
    return false;
}

bool LooksLikePathOrFilename(const std::wstring& s) {
    if (s.find(L":\\") != std::wstring::npos || s.find(L":/") != std::wstring::npos)
        return true;
    if (s.size() >= 5) {
        std::wstring lower = s;
        for (auto& c : lower) {
            if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
        }
        if (lower.find(L".xlsx") != std::wstring::npos
            || lower.find(L".xls") != std::wstring::npos
            || lower.find(L".docx") != std::wstring::npos
            || lower.find(L".csv") != std::wstring::npos
            || lower.find(L".txt") != std::wstring::npos) {
            return true;
        }
    }
    return false;
}

/// 另存为「浏览记录」这类短文件名：禁止当动态列表实例数据折叠掉
bool LooksLikeShortSaveFilename(const std::wstring& s) {
    const std::wstring t = Trim(s);
    if (t.empty() || t.size() > 20) return false;
    if (t.find(L'|') != std::wstring::npos) return false;
    if (t.find(L'\t') != std::wstring::npos || t.find(L'\n') != std::wstring::npos)
        return false;
    std::wstring lower = t;
    for (auto& c : lower) {
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    }
    if (lower.find(L"http://") != std::wstring::npos
        || lower.find(L"https://") != std::wstring::npos
        || lower.find(L"www.") != std::wstring::npos)
        return false;
    if (t.find(L'\\') != std::wstring::npos || t.find(L'/') != std::wstring::npos)
        return false;
    // 含数字更像列表序号/时间/标题后缀，不当文件名豁免（「浏览记录」无数字）
    for (wchar_t c : t) {
        if (c >= L'0' && c <= L'9') return false;
    }
    return true;
}

bool LooksLikeListWindowActivateMatch(const std::wstring& match) {
    std::wstring t = match;
    for (auto& c : t) {
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    }
    return t.find(L"edge") != std::wstring::npos
        || t.find(L"msedge") != std::wstring::npos
        || t.find(L"chrome") != std::wstring::npos
        || t.find(L"firefox") != std::wstring::npos
        || t.find(L"browser") != std::wstring::npos
        || t.find(L"历史") != std::wstring::npos
        || t.find(L"history") != std::wstring::npos
        || t.find(L"浏览器") != std::wstring::npos
        || t.find(L"bilibili") != std::wstring::npos;
}

bool TranscriptHasListWindow(const std::vector<AiLogicTranscriptEntry>& in) {
    for (const auto& e : in) {
        if (e.fromActivate && LooksLikeListWindowActivateMatch(e.activateMatch))
            return true;
        if (!e.fromActivate && e.action.remark.find(L"activateWindow:") != std::wstring::npos) {
            const size_t p = e.action.remark.find(L"activateWindow:");
            if (LooksLikeListWindowActivateMatch(e.action.remark.substr(p + 15)))
                return true;
        }
    }
    return false;
}

std::wstring FindLastListWindowMatch(const std::vector<AiLogicTranscriptEntry>& in) {
    for (int i = static_cast<int>(in.size()) - 1; i >= 0; --i) {
        const auto& e = in[static_cast<size_t>(i)];
        if (e.fromActivate && LooksLikeListWindowActivateMatch(e.activateMatch))
            return e.activateMatch;
        if (!e.fromActivate && e.action.remark.find(L"activateWindow:") != std::wstring::npos) {
            const size_t p = e.action.remark.find(L"activateWindow:");
            const std::wstring m = e.action.remark.substr(p + 15);
            if (LooksLikeListWindowActivateMatch(m)) return m;
        }
    }
    return {};
}

bool IsNavKeyAction(const ScriptAction& a) {
    if (a.type != ActionType::KeyClick) return false;
    std::wstring k = a.keyText;
    for (auto& c : k) {
        if (c >= L'a' && c <= L'z') c = static_cast<wchar_t>(c - L'a' + L'A');
    }
    return k == L"TAB" || k == L"ENTER" || k == L"RETURN" || k == L"HOME"
        || k == L"END" || k == L"UP" || k == L"DOWN" || k == L"LEFT" || k == L"RIGHT"
        || k == L"DELETE" || k == L"BACKSPACE" || k == L"ESCAPE" || k == L"ESC";
}

bool IsInstanceDataQuickInput(const ScriptAction& a) {
    if (a.type != ActionType::QuickInput) return false;
    const std::wstring t = Trim(a.inputText);
    if (t.empty()) return false;
    if (LooksLikeStaticHeaderText(t)) return false;
    if (LooksLikePathOrFilename(t)) return false;
    // 短文件名（另存为「浏览记录」）保留为固定输入，勿折成 OCR+AI
    if (LooksLikeShortSaveFilename(t)) return false;
    return true;
}

ScriptAction MakeOcrCapture(const std::wstring& varName, int indent,
    const std::wstring& listActivateMatch = {}) {
    ScriptAction a;
    a.type = ActionType::TextRecognition;
    a.searchFullScreen = true;
    a.ocrResultMode = 0; // 获取文字
    a.ocrFollowUp = 2;   // 保存变量
    a.matchVarName = varName;
    a.indent = indent;
    a.remark = L"文字识别：动态列表";
    if (!listActivateMatch.empty())
        a.remark += L"|listActivate:" + listActivateMatch;
    return a;
}

ScriptAction MakeDynamicFillAi(const std::wstring& taskPrompt, const std::wstring& ocrVar,
    int indent) {
    ScriptAction a;
    a.type = ActionType::AiActionExecute;
    a.aiWithImage = true;
    a.aiMaxSteps = 20; // 只填表：限制步数，防重开整任务
    a.aiLogicConvert = false; // 禁止嵌套再写回把动态数据再次写死
    a.aiContextMode = 1;
    a.indent = indent;
    a.remark = L"AI动作执行：动态列表填写";
    std::wstring p = L"【逻辑转化·动态数据·只填表】用变量 "
        + ocrVar + L"（上一拍文字识别，应来自列表/历史画面）解析条目，填入已打开的目标表格。"
        L"★★本步唯一目标：把列表条目写入表格。禁止新建工作簿、禁止另存为/保存对话框、"
        L"禁止打开/切换浏览器去重新取数、禁止从「保存 Excel」等前置步骤重开整任务。"
        L"★★禁止把某次运行见过的标题/时间/网址写死进 quickInput——必须解析 "
        + ocrVar + L" 或当前列表截图。"
        L"若 " + ocrVar + L" 像对话框/另存为 UI（含「另存为」「文件名」等）或费用中心等非列表页："
        L"先 activateWindow 到含列表的窗口，重新识别后再填；勿把错屏当数据。"
        L"表头若已存在勿重复写；用 Tab/Enter/Home 导航。";
    if (!taskPrompt.empty())
        p += L"\n背景说明（勿重做整段任务）：" + taskPrompt;
    a.aiPrompt = p;
    return a;
}

AiLogicTranscriptEntry MakeListActivateEntry(const std::wstring& match) {
    AiLogicTranscriptEntry e;
    e.fromActivate = true;
    e.activateMatch = match;
    e.action = MakeActivateSettle(match, 0);
    return e;
}

void PushOcrCaptureWithListActivate(
    std::vector<AiLogicTranscriptEntry>& out, const std::wstring& listMatch) {
    if (!listMatch.empty())
        out.push_back(MakeListActivateEntry(listMatch));
    AiLogicTranscriptEntry ocr;
    ocr.action = MakeOcrCapture(L"logicOcr", 0, listMatch);
    out.push_back(ocr);
}

bool LooksLikeSpreadsheetActivateMatch(const std::wstring& match) {
    std::wstring t = match;
    for (auto& c : t) {
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    }
    return t.find(L"excel") != std::wstring::npos
        || t.find(L"xlsx") != std::wstring::npos
        || t.find(L"xls") != std::wstring::npos
        || t.find(L"工作簿") != std::wstring::npos
        || t.find(L"wps") != std::wstring::npos
        || t.find(L"表格") != std::wstring::npos
        || t.find(L"et.exe") != std::wstring::npos;
}

/// 把「多次实例数据 quickInput（浏览记录标题等）」折叠为 OCR + 抽象 AI 填写，
/// 避免逻辑转化把某次运行看到的 10 条历史写死进脚本。
/// OCR 插在「切回表格」之前（列表仍在前台），避免在 Excel 格子上识别到空表/错屏。
std::vector<AiLogicTranscriptEntry> CollapseInstanceDataEntries(
    const std::vector<AiLogicTranscriptEntry>& in, const std::wstring& taskPrompt) {
    int instanceQi = 0;
    int dataStart = -1;
    for (int i = 0; i < static_cast<int>(in.size()); ++i) {
        const auto& e = in[static_cast<size_t>(i)];
        if (e.fromLocate || e.fromActivate) continue;
        if (IsInstanceDataQuickInput(e.action)) {
            ++instanceQi;
            if (dataStart < 0) dataStart = i;
        }
    }
    if (instanceQi < 4 || dataStart < 0) return in; // 少量固定输入保留（如文件名、确认词）
    // 无列表类窗（Edge/历史等）时勿折叠——避免另存为文件名被当成「动态数据」
    if (!TranscriptHasListWindow(in)) return in;
    const std::wstring listMatch = FindLastListWindowMatch(in);

    // 数据填写前最近一次切到表格 → OCR 紧插在其前（此时通常仍是列表窗）
    int excelActivateBeforeData = -1;
    for (int i = dataStart - 1; i >= 0; --i) {
        const auto& e = in[static_cast<size_t>(i)];
        if (!e.fromActivate) continue;
        if (LooksLikeSpreadsheetActivateMatch(e.activateMatch)) {
            excelActivateBeforeData = i;
            break;
        }
    }

    std::vector<AiLogicTranscriptEntry> out;
    out.reserve(in.size() + 3);
    bool collapsed = false;
    bool inDataRun = false;
    bool ocrInserted = false;
    for (size_t i = 0; i < in.size(); ++i) {
        const auto& e = in[i];
        if (excelActivateBeforeData >= 0
            && static_cast<int>(i) == excelActivateBeforeData && !ocrInserted) {
            // 先切回列表窗再 OCR，避免费用中心/错屏污染 logicOcr
            PushOcrCaptureWithListActivate(out, listMatch);
            ocrInserted = true;
            out.push_back(e); // 再切到 Excel
            inDataRun = false;
            continue;
        }
        if (e.fromLocate || e.fromActivate) {
            inDataRun = false;
            out.push_back(e);
            continue;
        }
        const bool instQi = IsInstanceDataQuickInput(e.action);
        const bool nav = IsNavKeyAction(e.action);
        if (!collapsed && instQi) {
            if (!inDataRun) {
                if (!ocrInserted) {
                    // 轨迹里没有「切回表格」：退化为填写前就地 OCR（先 activate 列表）
                    PushOcrCaptureWithListActivate(out, listMatch);
                    ocrInserted = true;
                }
                AiLogicTranscriptEntry fill;
                fill.action = MakeDynamicFillAi(taskPrompt, L"logicOcr", 0);
                out.push_back(fill);
                collapsed = true;
                inDataRun = true;
            }
            continue; // 丢掉写死的实例 quickInput
        }
        if (collapsed && inDataRun && (instQi || nav)) {
            // 数据行间的 Tab/Enter/Home 一并丢掉（由动态填写 AI 负责）
            continue;
        }
        inDataRun = false;
        out.push_back(e);
    }
    return out;
}

int FindDefineIndex(const std::vector<ScriptAction>& actions, const std::wstring& name) {
    for (int i = 0; i < static_cast<int>(actions.size()); ++i) {
        if (actions[static_cast<size_t>(i)].type == ActionType::DefineBlock
            && actions[static_cast<size_t>(i)].blockName == name) {
            return i;
        }
    }
    return -1;
}

bool IndexInsideAnyDefineBody(const std::vector<ScriptAction>& actions, int idx) {
    for (int i = 0; i < idx; ++i) {
        if (actions[static_cast<size_t>(i)].type != ActionType::DefineBlock) continue;
        const int end = ContainerBodyEnd(actions, i);
        if (idx > i && idx < end) return true;
    }
    return false;
}

int FindSourceAiIndex(const std::vector<ScriptAction>& actions, int originalNo,
    const std::wstring& prompt) {
    auto topLevelAi = [&](int i) -> bool {
        const auto& a = actions[static_cast<size_t>(i)];
        if (a.type != ActionType::AiActionExecute) return false;
        if (IndexInsideAnyDefineBody(actions, i)) return false;
        return true;
    };
    // originalNo 命中最优先（含顶层误标「逻辑转化回退」备注的 AI 步）
    if (originalNo > 0) {
        for (int i = 0; i < static_cast<int>(actions.size()); ++i) {
            if (!topLevelAi(i)) continue;
            if (actions[static_cast<size_t>(i)].originalNo == originalNo)
                return i;
        }
    }
    auto usablePrompt = [&](int i) -> bool {
        if (!topLevelAi(i)) return false;
        const auto& a = actions[static_cast<size_t>(i)];
        if (a.indent != 0) return false;
        // 按 prompt 模糊匹配时排除块内回退备注，防误伤
        if (a.remark.find(L"逻辑转化回退") != std::wstring::npos) return false;
        return true;
    };
    for (int i = 0; i < static_cast<int>(actions.size()); ++i) {
        if (!usablePrompt(i)) continue;
        const auto& a = actions[static_cast<size_t>(i)];
        if (a.aiLogicConvert && (prompt.empty() || a.aiPrompt == prompt))
            return i;
    }
    for (int i = 0; i < static_cast<int>(actions.size()); ++i) {
        if (!usablePrompt(i)) continue;
        if (actions[static_cast<size_t>(i)].aiPrompt == prompt)
            return i;
    }
    return -1;
}

void RenumberActions(std::vector<ScriptAction>& actions) {
    for (int i = 0; i < static_cast<int>(actions.size()); ++i)
        actions[static_cast<size_t>(i)].originalNo = i + 1;
}

std::wstring ToLowerCopy(std::wstring s) {
    for (auto& c : s) {
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    }
    return s;
}

bool ContainsAny(const std::wstring& hay, const std::initializer_list<const wchar_t*>& needles) {
    for (const wchar_t* n : needles) {
        if (hay.find(n) != std::wstring::npos) return true;
    }
    return false;
}

std::wstring PromptHashFrag(const std::wstring& prompt) {
    unsigned h = 2166136261u;
    for (wchar_t c : prompt) {
        h ^= static_cast<unsigned>(c & 0xFFFFu);
        h *= 16777619u;
    }
    wchar_t hex[16] = {};
    swprintf_s(hex, L"%06X", static_cast<unsigned>(h & 0xFFFFFFu));
    return hex;
}

}  // namespace

void AiLogicConvertSessionBegin(bool enabled, const std::wstring& blockName,
    const std::wstring& prompt, const std::wstring& scriptPath, int sourceOriginalNo,
    bool healPromote) {
    auto& s = Sess();
    s = LogicSession{};
    if (!enabled) return;
    s.active = true;
    s.healPromote = healPromote;
    s.blockName = blockName;
    s.prompt = prompt;
    s.scriptPath = scriptPath;
    s.sourceOriginalNo = sourceOriginalNo;
}

void AiLogicConvertSessionEnd() {
    Sess() = LogicSession{};
}

bool AiLogicConvertSessionActive() { return Sess().active; }
bool AiLogicConvertSessionIsHeal() { return Sess().active && Sess().healPromote; }
void AiLogicConvertSessionSetScriptPathIfEmpty(const std::wstring& scriptPath) {
    if (!Sess().active || scriptPath.empty()) return;
    if (Sess().scriptPath.empty()) Sess().scriptPath = scriptPath;
}
void AiLogicConvertMarkPendingWriteback(bool pending) {
    if (!Sess().active) return;
    Sess().pendingWriteback = pending;
}
bool AiLogicConvertPendingWriteback() {
    return Sess().active && Sess().pendingWriteback;
}
const std::wstring& AiLogicConvertSessionBlockName() { return Sess().blockName; }
const std::wstring& AiLogicConvertSessionPrompt() { return Sess().prompt; }
const std::wstring& AiLogicConvertSessionScriptPath() { return Sess().scriptPath; }
int AiLogicConvertSessionSourceNo() { return Sess().sourceOriginalNo; }

void AiLogicConvertNoteAction(const ScriptAction& a) {
    if (!Sess().active) return;
    if (!IsConvertibleType(a.type)) return;
    if (static_cast<int>(Sess().transcript.size()) >= 80) return;
    AiLogicTranscriptEntry e;
    e.action = a;
    Sess().transcript.push_back(std::move(e));
}

bool BitmapLooksLowFeature(HBITMAP bmp) {
    if (!bmp) return true;
    BITMAP bm{};
    if (!GetObjectW(bmp, sizeof(bm), &bm) || bm.bmWidth < 4 || bm.bmHeight < 4)
        return true;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = bm.bmWidth;
    bi.bmiHeader.biHeight = -bm.bmHeight; // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    const size_t n = static_cast<size_t>(bm.bmWidth) * static_cast<size_t>(bm.bmHeight);
    std::vector<uint32_t> px(n);
    HDC hdc = GetDC(nullptr);
    if (!hdc) return true;
    const int got = GetDIBits(hdc, bmp, 0, bm.bmHeight, px.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, hdc);
    if (got <= 0) return true;
    // 抽样灰度方差：表格空白网格线区方差极低
    double sum = 0, sum2 = 0;
    int samples = 0;
    const int step = (std::max)(1, static_cast<int>(n / 400));
    for (size_t i = 0; i < n; i += static_cast<size_t>(step)) {
        const uint32_t c = px[i];
        const double g = 0.299 * ((c >> 16) & 0xFF) + 0.587 * ((c >> 8) & 0xFF)
            + 0.114 * (c & 0xFF);
        sum += g;
        sum2 += g * g;
        ++samples;
    }
    if (samples < 8) return true;
    const double mean = sum / samples;
    const double var = sum2 / samples - mean * mean;
    return var < 180.0; // 经验阈值：空白格/纯色区
}

std::wstring CaptureAiLogicConvertTemplateAt(int screenX, int screenY) {
    // 调用方须在截取前 ScopedHideOwnUiForCapture（避免裁到宏调试窗）
    int vx = 0, vy = 0, vw = 0, vh = 0;
    GetVirtualScreenRect(vx, vy, vw, vh);
    // 默认更大裁剪；若像空白网格则再放大，尽量带上表头字母/行号等特征
    int half = 56;
    auto tryCapture = [&](int h) -> HBITMAP {
        const auto rect = ComputeClickCaptureRect(screenX, screenY, h,
            vx, vy, vx + vw, vy + vh);
        if (!rect.valid) return nullptr;
        return CaptureScreenRegion(rect.x1, rect.y1, rect.x2, rect.y2);
    };
    HBITMAP bmp = tryCapture(half);
    if (!bmp) return {};
    if (BitmapLooksLowFeature(bmp)) {
        DeleteBitmapHandle(bmp);
        half = 96;
        bmp = tryCapture(half);
        if (!bmp) return {};
        if (BitmapLooksLowFeature(bmp)) {
            DeleteBitmapHandle(bmp);
            half = 140;
            bmp = tryCapture(half);
            if (!bmp) return {};
        }
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    EnsureFindImagesDir();
    const std::wstring fileName = L"logic_" + std::to_wstring(ms) + L"_"
        + std::to_wstring(screenX) + L"_" + std::to_wstring(screenY) + L".bmp";
    const std::wstring absPath = FindImagesDir() + L"\\" + fileName;
    const bool ok = SaveBitmapToFile(bmp, absPath);
    DeleteBitmapHandle(bmp);
    if (!ok) return {};
    return EnsureImageInLibrary(absPath);
}

void AiLogicConvertNoteLocate(const std::wstring& target, int screenX, int screenY,
    const std::wstring& button, int clickCount, const std::wstring& templatePath) {
    if (!Sess().active) return;
    if (static_cast<int>(Sess().transcript.size()) >= 80) return;
    AiLogicTranscriptEntry e;
    e.fromLocate = true;
    e.locateTarget = target;
    e.templatePath = templatePath;
    e.screenX = screenX;
    e.screenY = screenY;
    e.action.type = ActionType::FindImage;
    e.action.imagePath = templatePath;
    e.action.button = (button == L"right") ? MouseButtonType::Right : MouseButtonType::Left;
    e.action.clickCount = std::max(1, clickCount);
    e.action.remark = target;
    Sess().transcript.push_back(std::move(e));
}

void AiLogicConvertNoteWindowActivate(const std::wstring& matchQuery) {
    if (!Sess().active) return;
    const std::wstring match = Trim(matchQuery);
    if (match.empty()) return;
    if (static_cast<int>(Sess().transcript.size()) >= 80) return;
    // 去重：连续同 match 的切窗只记一次
    if (!Sess().transcript.empty()) {
        const auto& last = Sess().transcript.back();
        if (last.fromActivate && last.activateMatch == match) return;
    }
    AiLogicTranscriptEntry e;
    e.fromActivate = true;
    e.activateMatch = match;
    e.action = MakeActivateSettle(match, 0);
    Sess().transcript.push_back(std::move(e));
}

void AiLogicConvertNoteOpenWebpage(const std::wstring& url) {
    if (!Sess().active) return;
    const std::wstring u = Trim(url);
    if (u.empty()) return;
    if (static_cast<int>(Sess().transcript.size()) >= 80) return;
    if (!Sess().transcript.empty()) {
        const auto& last = Sess().transcript.back();
        if (!last.fromLocate && !last.fromActivate
            && last.action.type == ActionType::OpenWebpage
            && last.action.targetPath == u) {
            return;
        }
    }
    ScriptAction a;
    a.type = ActionType::OpenWebpage;
    a.targetPath = u;
    a.remark = L"扩展导航";
    AiLogicConvertNoteAction(a);
}

const std::vector<AiLogicTranscriptEntry>& AiLogicConvertTranscript() {
    return Sess().transcript;
}

bool AiLogicConvertLooksLikeFailedComplete(const std::wstring& text) {
    if (text.empty()) return false;
    const std::wstring t = ToLowerCopy(text);
    return ContainsAny(t, {
        L"未找到", L"找不到", L"失败", L"卡点", L"未完成", L"无法",
        L"abort", L"failed", L"error", L"not found", L"无法完成",
        L"没有完成", L"做不到", L"中止", L"取消",
    });
}

bool AiLogicConvertTranscriptHasConvertible() {
    for (const auto& e : Sess().transcript) {
        if (e.fromLocate && !e.templatePath.empty()) return true;
        if (e.fromActivate && !e.activateMatch.empty()) return true;
        if (!e.fromLocate && !e.fromActivate && IsConvertibleType(e.action.type))
            return true;
    }
    return false;
}

bool AiLogicConvertShouldWriteback(bool apiOk, bool actionsAlreadyExecuted,
    bool anyActionsExecuted, const std::wstring& completeReason,
    const std::wstring& textResult) {
    if (!apiOk || !actionsAlreadyExecuted) return false;
    if (!anyActionsExecuted) return false;
    if (AiLogicConvertLooksLikeFailedComplete(completeReason)) return false;
    if (AiLogicConvertLooksLikeFailedComplete(textResult)) return false;
    if (!AiLogicConvertTranscriptHasConvertible()) return false;
    return true;
}

std::wstring MakeAiLogicBlockName(const std::wstring& preferred, const std::wstring& prompt) {
    auto sanitize = [](const std::wstring& s, bool asciiOnly) {
        std::wstring o;
        for (wchar_t c : s) {
            if (asciiOnly) {
                if ((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z')
                    || (c >= L'0' && c <= L'9')) {
                    o.push_back(c);
                }
            } else if (iswalnum(c)) {
                // 保留用户指定中文块名（IsValidBlockName 用 iswalpha/iswalnum）
                o.push_back(c);
            }
        }
        while (!o.empty() && iswdigit(o[0])) o.erase(o.begin());
        if (o.size() > 28) o.resize(28);
        return o;
    };
    if (!preferred.empty()) {
        std::wstring pref = sanitize(preferred, false);
        if (IsValidBlockName(pref)) return pref;
        pref = sanitize(preferred, true);
        if (IsValidBlockName(pref)) return pref;
    }
    std::wstring frag;
    for (wchar_t c : prompt) {
        if (frag.size() >= 12) break;
        if ((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z')
            || (c >= L'0' && c <= L'9')) {
            frag.push_back(c);
        }
    }
    // 中文/CJK 任务名：用稳定哈希，避免全部塌成 LogicTask 互相串块
    if (frag.empty()) frag = PromptHashFrag(prompt);
    std::wstring base = L"Logic" + sanitize(frag, true);
    if (!IsValidBlockName(base)) base = L"Logic" + PromptHashFrag(prompt);
    if (!IsValidBlockName(base)) base = L"LogicTask";
    return base;
}

bool ScriptHasDefineBlock(const std::vector<ScriptAction>& actions, const std::wstring& blockName) {
    return FindDefineIndex(actions, blockName) >= 0;
}

std::wstring BriefActionTypeName(const AiLogicTranscriptEntry& e) {
    if (e.fromLocate) {
        std::wstring t = Trim(e.locateTarget);
        if (t.size() > 24) t = t.substr(0, 24) + L"…";
        return t.empty() ? L"找图点击" : (L"找图:" + t);
    }
    if (e.fromActivate) {
        std::wstring t = Trim(e.activateMatch);
        if (t.size() > 28) t = t.substr(0, 28) + L"…";
        return t.empty() ? L"切窗" : (L"切窗:" + t);
    }
    switch (e.action.type) {
    case ActionType::RunProgram: return L"打开程序";
    case ActionType::HotkeyShortcut: return L"快捷键";
    case ActionType::QuickInput: {
        std::wstring t = Trim(e.action.inputText);
        if (t.size() > 16) t = t.substr(0, 16) + L"…";
        return t.empty() ? L"快捷输入" : (L"输入:" + t);
    }
    case ActionType::KeyClick: {
        std::wstring k = Trim(e.action.keyText);
        return k.empty() ? L"按键" : (L"按键:" + k);
    }
    case ActionType::ScrollWheel: return L"滚轮";
    case ActionType::OpenWebpage: return L"打开网页";
    case ActionType::OpenFile: return L"打开文件";
    case ActionType::ActivateWindow: return L"激活窗口";
    case ActionType::TextRecognition: return L"文字识别";
    case ActionType::AiActionExecute: return L"AI动作";
    case ActionType::Wait: return L"等待";
    case ActionType::FindImage: return L"找图";
    default: return L"其它";
    }
}

std::wstring JoinBriefSummaries(const std::vector<AiLogicTranscriptEntry>& entries,
    int from, int to, int maxItems) {
    std::wstring out;
    int n = 0;
    for (int i = from; i < to && n < maxItems; ++i) {
        const auto& e = entries[static_cast<size_t>(i)];
        if (e.action.type == ActionType::Wait && !e.fromActivate) continue;
        if (!out.empty()) out += L" → ";
        out += BriefActionTypeName(e);
        ++n;
    }
    if (to - from > maxItems && !out.empty()) out += L" → …";
    return out;
}

std::wstring BuildLocateFallbackPrompt(
    const std::wstring& taskPrompt,
    const std::vector<AiLogicTranscriptEntry>& entries,
    const std::vector<int>& locateIdxs,
    int locOrd) {
    const int idx = locateIdxs[static_cast<size_t>(locOrd)];
    const auto& loc = entries[static_cast<size_t>(idx)];
    const int nextStart = idx + 1;
    const int nextEnd = (locOrd + 1 < static_cast<int>(locateIdxs.size()))
        ? locateIdxs[static_cast<size_t>(locOrd + 1)]
        : static_cast<int>(entries.size());

    std::wstring p = L"【逻辑转化回退·自愈】你是快路径找图失败后的补洞助手，不是从头执行整任务。\n";
    if (!taskPrompt.empty())
        p += L"原任务（仅供理解，禁止从零重做）：" + taskPrompt + L"\n";
    p += L"失败锚点：" + (loc.locateTarget.empty() ? L"（未命名找图）" : loc.locateTarget) + L"\n";

    const std::wstring done = JoinBriefSummaries(entries, 0, idx, 10);
    p += L"已完成（本分支前快路径）：";
    p += done.empty() ? L"（尚无，可能卡在第一步找图）" : done;
    p += L"\n";

    std::wstring nextPart = JoinBriefSummaries(entries, nextStart, nextEnd, 8);
    if (locOrd + 1 < static_cast<int>(locateIdxs.size())) {
        const auto& nxtLoc = entries[static_cast<size_t>(
            locateIdxs[static_cast<size_t>(locOrd + 1)])];
        if (!nextPart.empty()) nextPart += L" → ";
        nextPart += BriefActionTypeName(nxtLoc);
        nextPart += L"（及后续）";
    }
    p += L"下一步（找图成功后本应继续）：";
    p += nextPart.empty() ? L"（本段末尾，修好锚点后即可 completeTask）" : nextPart;
    p += L"\n";

    p += L"硬约束：先看截图与前台窗口标题判断真实进度；已完成的保存/建表/打开勿重复；"
         L"只补当前缺口并接到「下一步」。"
         L"卡在对话框：以截图与按钮文案为准，灰钮先改前置条件。"
         L"列表数据须在列表窗识别，禁止把某次历史标题/时间写死进 quickInput。"
         L"修好后 completeTask；禁止 updateTaskMemo 把 goal 重置成整段任务从头开始。";
    return p;
}

AiLogicConvertCompileResult CompileAiLogicConvert(const AiLogicConvertCompileInput& in) {
    AiLogicConvertCompileResult out;
    if (in.transcript.empty()) {
        out.error = L"轨迹为空，跳过逻辑转化写回";
        return out;
    }

    out.blockName = MakeAiLogicBlockName(in.preferredBlockName, in.taskPrompt);
    const std::wstring timerVar = L"logicT";

    std::vector<AiLogicTranscriptEntry> entries;
    entries.reserve(in.transcript.size());
    for (const auto& e : in.transcript) {
        if (static_cast<int>(entries.size()) >= in.maxNewSteps) break;
        if (e.fromLocate) {
            if (e.templatePath.empty()) continue;
            entries.push_back(e);
            continue;
        }
        if (e.fromActivate) {
            if (e.activateMatch.empty()) continue;
            entries.push_back(e);
            continue;
        }
        // 过长 Wait 会干扰步间节奏；找图限时本身承担同步
        if (e.action.type == ActionType::Wait && e.action.duration > 2.0) continue;
        if (IsConvertibleType(e.action.type)) entries.push_back(e);
    }
    // 动态列表（历史记录等）勿写死 quickInput 正文 → OCR + 抽象 AI 填写
    entries = CollapseInstanceDataEntries(entries, in.taskPrompt);
    if (entries.empty()) {
        out.error = L"无可固化步骤（需键盘/热键/定位模板等）";
        return out;
    }

    std::vector<int> locateIdxs;
    for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
        if (entries[static_cast<size_t>(i)].fromLocate
            && !entries[static_cast<size_t>(i)].templatePath.empty()) {
            locateIdxs.push_back(i);
        }
    }

    auto appendPlain = [](std::vector<ScriptAction>& dst, const AiLogicTranscriptEntry& e,
        int indent) {
        if (e.fromActivate) {
            dst.push_back(MakeActivateSettle(e.activateMatch, indent));
            return;
        }
        ScriptAction a = e.action;
        if (a.type == ActionType::Wait
            || a.type == ActionType::TextRecognition
            || a.type == ActionType::FindImage) {
            a.customText.clear();
        }
        dst.push_back(MakeIndent(a, indent));
    };

    auto appendRangeWithGaps = [&](std::vector<ScriptAction>& dst, int from, int to,
        int indent) {
        bool first = true;
        for (int i = from; i < to; ++i) {
            const auto& e = entries[static_cast<size_t>(i)];
            if (e.fromLocate) continue;
            if (!first) dst.push_back(MakeGapWait(indent, GapWaitSecFor(e)));
            first = false;
            appendPlain(dst, e, indent);
        }
    };

    const std::wstring fbBase = in.taskPrompt.empty()
        ? L"【逻辑转化回退·自愈】修复当前界面漂移，使后续宏步骤可继续。"
          L"先看截图与窗口标题判断卡在哪一步，只补缺口，勿从零重做整个任务。"
          L"修好后 completeTask；勿把 goal 重置成整段任务。"
        : (L"【逻辑转化回退·自愈】原任务（仅供理解，禁止从零重做）：" + in.taskPrompt
            + L"\n当前快路径失败。先看截图与窗口标题判断已完成到哪一步，"
              L"只修复漂移并接到后续步骤；勿从零重开、勿重复已完成的保存/建表。"
              L"若卡在对话框：看图与按钮，灰钮先改前置；勿盲猜应用快捷键。"
              L"若需列表数据：切到列表窗识别后再填表，禁止写死历史标题/时间。"
              L"勿 updateTaskMemo 把 goal 重置成整段任务从头开始。");

    std::vector<ScriptAction> body;

    // 无找图：保留轻量 if 骨架（timer≈0 走快路径）+ else AI
    if (locateIdxs.empty()) {
        body.push_back(MakeTimer(timerVar, 1));
        body.push_back(MakeIf(timerVar + L" < 8", 1));
        appendRangeWithGaps(body, 0, static_cast<int>(entries.size()), 2);
        body.push_back(MakeElse(1));
        std::wstring fb = fbBase;
        const std::wstring done = JoinBriefSummaries(entries, 0, static_cast<int>(entries.size()), 10);
        if (!done.empty())
            fb += L"\n已完成（快路径摘要）：" + done;
        body.push_back(MakeFallbackAi(fb, out.blockName, 2));
    } else {
        // 首个找图前的键盘/切窗等
        appendRangeWithGaps(body, 0, locateIdxs[0], 1);

        // 每个找图：复用 logicT 重新打点 → 限时找图点击 → if(timer<上限) 成功分支 else AI
        // 嵌套：下一找图写在上一成功分支内，避免「整段共用一个计时器」
        std::function<void(int, int)> emitLocateChain;
        emitLocateChain = [&](int locOrd, int indent) {
            const int idx = locateIdxs[static_cast<size_t>(locOrd)];
            const auto& loc = entries[static_cast<size_t>(idx)];
            const int limitSec = LocateFindTimeSec(loc);
            const std::wstring limitStr = std::to_wstring(limitSec);

            body.push_back(MakeTimer(timerVar, indent));
            body.push_back(MakeFindImageClick(loc.templatePath, indent,
                loc.action.button == MouseButtonType::Right ? L"right" : L"left",
                loc.action.clickCount,
                CanonicalLocateRemark(loc.locateTarget, false),
                limitStr, loc.screenX, loc.screenY));
            body.push_back(MakeIf(timerVar + L" < " + limitStr, indent));

            const int nextStart = idx + 1;
            const int nextEnd = (locOrd + 1 < static_cast<int>(locateIdxs.size()))
                ? locateIdxs[static_cast<size_t>(locOrd + 1)]
                : static_cast<int>(entries.size());
            const bool hasNextLocate = locOrd + 1 < static_cast<int>(locateIdxs.size());
            // 找图点击成功后至少插一步间 Wait，避免与后续动作粘连
            if (nextStart < nextEnd || hasNextLocate) {
                double gap = 0.05;
                if (nextStart < nextEnd)
                    gap = GapWaitSecFor(entries[static_cast<size_t>(nextStart)]);
                else
                    gap = GapWaitSecFor(entries[static_cast<size_t>(
                        locateIdxs[static_cast<size_t>(locOrd + 1)])]);
                body.push_back(MakeGapWait(indent + 1, gap));
            }
            appendRangeWithGaps(body, nextStart, nextEnd, indent + 1);

            if (hasNextLocate) {
                emitLocateChain(locOrd + 1, indent + 1);
            }

            body.push_back(MakeElse(indent));
            body.push_back(MakeFallbackAi(
                BuildLocateFallbackPrompt(in.taskPrompt, entries, locateIdxs, locOrd),
                out.blockName, indent + 1));
        };
        emitLocateChain(0, 1);
    }

    out.defineAndBody.clear();
    out.defineAndBody.push_back(MakeDefine(out.blockName));
    for (auto& step : body) out.defineAndBody.push_back(std::move(step));
    out.runBlock = MakeRun(out.blockName);
    out.ok = true;
    out.summary = L"逻辑转化：块 " + out.blockName + L"，快路径约 "
        + std::to_wstring(entries.size()) + L" 步（找图分支 "
        + std::to_wstring(locateIdxs.size()) + L" 处）"
        + (in.promoteExisting ? L"（提升合并）" : L"（首次生成）")
        + L"；回退Prompt含进度上下文（旧块需再录/提升合并才更新）";
    return out;
}

bool ApplyAiLogicConvertWriteback(const AiLogicConvertWritebackRequest& req, std::wstring& err) {
    err.clear();
    if (!req.compiled.ok) {
        err = req.compiled.error.empty() ? L"编译失败" : req.compiled.error;
        return false;
    }
    if (req.scriptPath.empty()
        || GetFileAttributesW(req.scriptPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        err = L"脚本路径无效，无法写回";
        return false;
    }

    ScriptFileData data = LoadScriptFileData(req.scriptPath, false);
    auto& actions = data.actions;
    const std::wstring& blockName = req.compiled.blockName;
    const int defIdx = FindDefineIndex(actions, blockName);
    const bool promote = defIdx >= 0;

    if (promote) {
        // 嵌套「每找图一分支」结构下，增量往第一层 if 里塞步骤易错位。
        // 提升写回：用新编译整块替换 Define 体（保留块名），模板/步骤以本次轨迹为准。
        const int bodyEnd = ContainerBodyEnd(actions, defIdx);
        const auto& neu = req.compiled.defineAndBody;
        if (neu.empty() || neu[0].type != ActionType::DefineBlock) {
            err = L"新编译块无效，拒绝提升写回";
            return false;
        }
        actions.erase(actions.begin() + defIdx, actions.begin() + bodyEnd);
        actions.insert(actions.begin() + defIdx, neu.begin(), neu.end());
    } else {
        actions.insert(actions.begin(),
            req.compiled.defineAndBody.begin(), req.compiled.defineAndBody.end());
        const int shift = static_cast<int>(req.compiled.defineAndBody.size());
        int src = FindSourceAiIndex(actions, req.sourceOriginalNo, req.sourcePrompt);
        if (src < 0) {
            for (int i = shift; i < static_cast<int>(actions.size()); ++i) {
                const auto& a = actions[static_cast<size_t>(i)];
                if (a.type == ActionType::AiActionExecute && a.aiPrompt == req.sourcePrompt
                    && a.indent == 0
                    && a.remark.find(L"逻辑转化回退") == std::wstring::npos
                    && !IndexInsideAnyDefineBody(actions, i)) {
                    src = i;
                    break;
                }
            }
        }
        if (src < 0) {
            err = L"找不到要替换的 AI 动作执行步骤";
            return false;
        }
        ScriptAction run = req.compiled.runBlock;
        run.originalNo = actions[static_cast<size_t>(src)].originalNo;
        actions[static_cast<size_t>(src)] = run;
    }

    RenumberActions(actions);
    if (!SaveScriptFileData(req.scriptPath, data)) {
        err = L"保存脚本失败";
        return false;
    }
    return true;
}

bool TryFlushAiLogicConvertSession(const std::wstring& pathFallback,
    std::wstring& summaryOut, std::wstring& errOut) {
    summaryOut.clear();
    errOut.clear();
    if (!Sess().active) {
        errOut = L"会话未激活";
        return false;
    }
    if (!Sess().pendingWriteback && !AiLogicConvertTranscriptHasConvertible()) {
        errOut = L"无可写回内容";
        return false;
    }
    if (Sess().scriptPath.empty() && !pathFallback.empty())
        Sess().scriptPath = pathFallback;
    if (Sess().scriptPath.empty()) {
        errOut = L"脚本路径为空";
        return false;
    }
    AiLogicConvertCompileInput cin;
    cin.taskPrompt = Sess().prompt;
    cin.preferredBlockName = Sess().blockName;
    cin.transcript = Sess().transcript;
    const std::wstring blk = MakeAiLogicBlockName(cin.preferredBlockName, cin.taskPrompt);
    // 指定块名：有则提升合并，无则首次创建（heal/回退会话同样适用，勿因块缺失直接放弃写回）
    cin.promoteExisting = ScriptHasDefineBlock(
        LoadScriptFileData(Sess().scriptPath, false).actions, blk);
    auto compiled = CompileAiLogicConvert(cin);
    if (!compiled.ok) {
        errOut = compiled.error.empty() ? L"编译失败" : compiled.error;
        return false;
    }
    AiLogicConvertWritebackRequest req;
    req.scriptPath = Sess().scriptPath;
    req.sourceOriginalNo = Sess().sourceOriginalNo;
    req.sourcePrompt = Sess().prompt;
    req.compiled = std::move(compiled);
    if (!ApplyAiLogicConvertWriteback(req, errOut)) {
        if (errOut.empty()) errOut = L"写回失败";
        return false;
    }
    summaryOut = req.compiled.summary;
    Sess().pendingWriteback = false;
    return true;
}

bool UserExplicitlyRequestsAiActionExecute(const std::wstring& userText) {
    const std::wstring t = ToLowerCopy(userText);
    return ContainsAny(t, {
        L"ai动作执行", L"ai 动作执行", L"动作执行",
        L"让ai自动", L"让 ai 自动", L"自动操作桌面",
        L"aiactionexecute",
    });
}

bool UserExplicitlyRequestsLogicConvert(const std::wstring& userText) {
    const std::wstring t = ToLowerCopy(userText);
    return ContainsAny(t, {
        L"逻辑转化", L"logic convert", L"logicconvert",
        L"自愈脚本", L"可自愈", L"编译成宏",
    });
}

namespace {
std::wstring& AgentUserCtx() {
    static thread_local std::wstring s;
    return s;
}
}  // namespace

void SetAgentToolUserContext(const std::wstring& lastUserText) {
    AgentUserCtx() = lastUserText;
}

std::wstring GetAgentToolUserContext() {
    return AgentUserCtx();
}

int StripUnauthorizedLogicConvert(std::vector<ScriptAction>& actions) {
    if (UserExplicitlyRequestsLogicConvert(AgentUserCtx())) return 0;
    int n = 0;
    for (auto& a : actions) {
        if (a.type == ActionType::AiActionExecute && a.aiLogicConvert) {
            a.aiLogicConvert = false;
            a.aiLogicBlockName.clear();
            ++n;
        }
    }
    return n;
}
