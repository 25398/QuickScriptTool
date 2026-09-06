#include "ai_action_lookahead.h"

#include "macro_execute_tools.h"

#include <algorithm>

namespace {

AiLookaheadDefaultFetchFn& DefaultFetchSlot() {
    static AiLookaheadDefaultFetchFn fn;
    return fn;
}

std::wstring TruncateW(std::wstring s, size_t maxChars) {
    if (s.size() <= maxChars) return s;
    s.resize(maxChars);
    s += L"…";
    return s;
}

std::wstring BuildLookaheadUserPrompt(
    const std::wstring& memoText, const std::wstring& toolBatchSummary) {
    return L"刚执行完一批工具（结果摘要）：\n"
        + TruncateW(toolBatchSummary.empty() ? L"（无摘要）" : toolBatchSummary, 900)
        + L"\n\n任务备忘：\n"
        + TruncateW(memoText.empty() ? L"（空）" : memoText, 800)
        + L"\n\n请只规划下一步工具调用（假设界面已按预期推进）。勿重复上批已成功动作。";
}

}  // namespace

void RegisterAiLookaheadDefaultFetch(AiLookaheadDefaultFetchFn fn) {
    DefaultFetchSlot() = std::move(fn);
}

std::wstring FormatAiLookaheadHint(const AiLookaheadCachedPlan& plan) {
    if (!plan.ready || plan.toolCalls.empty()) return {};
    std::wstring s = L"宿主预规划下一步（未执行，以当前观察为准；不符则丢弃重规划）：";
    if (!plan.summary.empty()) {
        s += plan.summary;
        return s;
    }
    int n = 0;
    for (const auto& tc : plan.toolCalls) {
        if (n >= 4) {
            s += L"…";
            break;
        }
        if (n) s += L"; ";
        s += tc.name;
        if (!tc.arguments.empty()) {
            std::wstring args = tc.arguments;
            if (args.size() > 60) {
                args.resize(60);
                args += L"…";
            }
            s += L"(" + args + L")";
        }
        ++n;
    }
    return s;
}

std::wstring SummarizeAiToolBatchForLookahead(
    const std::vector<ChatMessage>& history, size_t afterIdx) {
    std::wstring s;
    for (size_t i = afterIdx; i < history.size(); ++i) {
        const auto& m = history[i];
        if (m.role == L"assistant") {
            for (const auto& tc : m.tool_calls) {
                if (!s.empty()) s += L"; ";
                s += tc.name;
            }
        } else if (m.role == L"tool") {
            std::wstring body = m.content;
            if (body.size() > 120) {
                body.resize(120);
                body += L"…";
            }
            if (!s.empty()) s += L" → ";
            s += body;
        }
        if (s.size() > 900) {
            s.resize(900);
            s += L"…";
            break;
        }
    }
    return s;
}

bool ShouldDiscardAiLookahead(
    bool observeUnchanged, bool onlyDynamicChanged, bool dialogBlocking) {
    if (dialogBlocking) return true;
    if (observeUnchanged && !onlyDynamicChanged) return true;
    return false;
}

AiActionLookahead::~AiActionLookahead() {
    Cancel();
}

void AiActionLookahead::JoinWorker_() {
    if (worker_.joinable()) {
        try {
            worker_.join();
        } catch (...) {
        }
    }
}

void AiActionLookahead::Cancel() {
    cancelWorker_.store(true);
    JoinWorker_();
    running_.store(false);
    cancelWorker_.store(false);
    std::lock_guard<std::mutex> lock(mu_);
    inFlightPrefetch_ = false;
}

void AiActionLookahead::Reset() {
    Cancel();
    std::lock_guard<std::mutex> lock(mu_);
    cached_ = {};
    fetchOverride_ = nullptr;
}

void AiActionLookahead::SetFetchOverrideForTest(AiLookaheadFetchFn fn) {
    std::lock_guard<std::mutex> lock(mu_);
    fetchOverride_ = std::move(fn);
}

void AiActionLookahead::SetCachedForTest(AiLookaheadCachedPlan plan) {
    std::lock_guard<std::mutex> lock(mu_);
    cached_ = std::move(plan);
    cached_.ready = true;
}

bool AiActionLookahead::IsRunningForTest() const {
    return running_.load();
}

void AiActionLookahead::BeginAfterTools(
    const AgentConfig& config,
    const std::wstring& memoText,
    const std::wstring& toolBatchSummary,
    const std::atomic_bool& stopFlag,
    AiHttpAbortSlot* httpAbort) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (inFlightPrefetch_ || running_.load()) return;
        inFlightPrefetch_ = true;
        cached_ = {};
    }
    cancelWorker_.store(true);
    JoinWorker_();
    cancelWorker_.store(false);
    if (stopFlag.load()) {
        std::lock_guard<std::mutex> lock(mu_);
        inFlightPrefetch_ = false;
        return;
    }

    const std::wstring userPrompt = BuildLookaheadUserPrompt(memoText, toolBatchSummary);
    AiLookaheadFetchFn overrideFn;
    {
        std::lock_guard<std::mutex> lock(mu_);
        overrideFn = fetchOverride_;
    }
    AiLookaheadDefaultFetchFn defaultFn = DefaultFetchSlot();

    const std::atomic_bool* stopPtr = &stopFlag;
    running_.store(true);
    worker_ = std::thread([this, config, userPrompt, stopPtr, httpAbort, overrideFn, defaultFn]() {
        std::vector<ToolCallRecord> calls;
        try {
            if (overrideFn) {
                calls = overrideFn(userPrompt, *stopPtr);
            } else if (defaultFn) {
                calls = defaultFn(config, userPrompt, *stopPtr, httpAbort, cancelWorker_);
            }
        } catch (...) {
            calls.clear();
        }

        AiLookaheadCachedPlan plan;
        plan.ready = !calls.empty() && !stopPtr->load() && !cancelWorker_.load();
        plan.toolCalls = std::move(calls);
        if (plan.ready) {
            int n = 0;
            for (const auto& tc : plan.toolCalls) {
                if (n) plan.summary += L"; ";
                plan.summary += tc.name;
                if (++n >= 4) break;
            }
        }
        {
            std::lock_guard<std::mutex> lock(mu_);
            cached_ = std::move(plan);
            inFlightPrefetch_ = false;
        }
        running_.store(false);
    });
}

AiLookaheadCachedPlan AiActionLookahead::WaitReady_(int waitMs) {
    const int slice = 50;
    int waited = 0;
    while (running_.load() && waited < waitMs) {
        Sleep(static_cast<DWORD>(slice));
        waited += slice;
    }
    if (running_.load()) {
        Cancel();
        return {};
    }
    JoinWorker_();
    std::lock_guard<std::mutex> lock(mu_);
    return cached_;
}

std::wstring AiActionLookahead::TakeHintAfterObserve(
    bool observeUnchanged,
    bool onlyDynamicChanged,
    bool dialogBlocking,
    int waitMs) {
    AiLookaheadCachedPlan plan = WaitReady_(waitMs);
    if (ShouldDiscardAiLookahead(observeUnchanged, onlyDynamicChanged, dialogBlocking)) {
        std::lock_guard<std::mutex> lock(mu_);
        cached_ = {};
        return {};
    }
    const std::wstring hint = FormatAiLookaheadHint(plan);
    {
        std::lock_guard<std::mutex> lock(mu_);
        cached_ = {};
    }
    return hint;
}

std::wstring AiActionLookahead::TakeHintSkipObserve(int waitMs) {
    AiLookaheadCachedPlan plan = WaitReady_(waitMs);
    const std::wstring hint = FormatAiLookaheadHint(plan);
    {
        std::lock_guard<std::mutex> lock(mu_);
        cached_ = {};
    }
    return hint;
}
