#pragma once
// Speculative next-step planning: prefetch tool_calls while local settle/capture runs.
// Never auto-executes; caller validates after observe then injects a hint into the next round.

#include "agent_core.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/// Optional test/override: returns dry-run tool_calls for a lookahead user prompt.
using AiLookaheadFetchFn = std::function<std::vector<ToolCallRecord>(
    const std::wstring& userPrompt, const std::atomic_bool& stopFlag)>;

/// Full fetch with API config (product registers default; selftest uses SetFetchOverrideForTest).
using AiLookaheadDefaultFetchFn = std::function<std::vector<ToolCallRecord>(
    const AgentConfig& config,
    const std::wstring& userPrompt,
    const std::atomic_bool& stopFlag,
    AiHttpAbortSlot* httpAbort,
    const std::atomic_bool& cancelWorker)>;

/// Register product default (call once from ai_action_service).
void RegisterAiLookaheadDefaultFetch(AiLookaheadDefaultFetchFn fn);

/// 预规划等待上限：观察后 400ms、SKIP_OBSERVE 150ms。
/// 预规划是「猜测」，绝不值得让主链路等它：预取通常在观察/settle 的 0.3–2s 里就跑完，
/// 跑不完就以当前画面为准丢弃（对齐 Midscene「规划以最新观察为准」）。
inline constexpr int kAiLookaheadAfterObserveWaitMs = 400;
inline constexpr int kAiLookaheadSkipObserveWaitMs = 150;
/// 单次 AI 动作执行最多做几次预规划（每次都是一次完整 API 请求）
inline constexpr int kAiLookaheadMaxStartsPerAction = 2;

struct AiLookaheadCachedPlan {
    bool ready = false;
    std::vector<ToolCallRecord> toolCalls;
    std::wstring summary;
};

/// Format cached tool_calls into a short next-round instruction hint (may be empty).
std::wstring FormatAiLookaheadHint(const AiLookaheadCachedPlan& plan);

/// Summarize assistant tool_calls + tool results after histBefore for lookahead input.
std::wstring SummarizeAiToolBatchForLookahead(
    const std::vector<ChatMessage>& history, size_t afterIdx);

/// Whether observe/dialog state should discard a speculative plan.
bool ShouldDiscardAiLookahead(
    bool observeUnchanged, bool onlyDynamicChanged, bool dialogBlocking);

class AiActionLookahead {
public:
    AiActionLookahead() = default;
    ~AiActionLookahead();

    AiActionLookahead(const AiActionLookahead&) = delete;
    AiActionLookahead& operator=(const AiActionLookahead&) = delete;

    void Cancel();
    void Reset();

    /// Begin background prefetch (at most one). Overlaps with settle/capture.
    void BeginAfterTools(
        const AgentConfig& config,
        const std::wstring& memoText,
        const std::wstring& toolBatchSummary,
        const std::atomic_bool& stopFlag,
        AiHttpAbortSlot* httpAbort);

    /// Wait briefly for prefetch; discard if unacceptable; return hint for next instruction.
    /// waitMs 只吃「观察/settle 期间已经跑完」的红利：超时即丢弃并真正中断预取，
    /// 不再让 join 卡在在途 API 上（旧实现 wait 6s + join 最长可再等 25s）。
    std::wstring TakeHintAfterObserve(
        bool observeUnchanged,
        bool onlyDynamicChanged,
        bool dialogBlocking,
        int waitMs = kAiLookaheadAfterObserveWaitMs);

    /// For SKIP_OBSERVE rounds: take hint if ready (no UI surprise assumed).
    std::wstring TakeHintSkipObserve(int waitMs = kAiLookaheadSkipObserveWaitMs);

    /// 上一次 Take 是否因为预取未就绪而丢弃（调用方据此记日志/统计）
    bool LastTakeDiscardedNotReady() const { return lastTakeDiscarded_; }

    void SetFetchOverrideForTest(AiLookaheadFetchFn fn);
    void SetCachedForTest(AiLookaheadCachedPlan plan);
    bool IsRunningForTest() const;

private:
    void JoinWorker_();
    AiLookaheadCachedPlan WaitReady_(int waitMs);

    mutable std::mutex mu_;
    std::thread worker_;
    std::atomic_bool running_{false};
    std::atomic_bool cancelWorker_{false};
    AiLookaheadCachedPlan cached_;
    AiLookaheadFetchFn fetchOverride_;
    /// 预取专用 HTTP 中断槽：绝不能复用主链路的槽（复用会把主请求也一起掐掉），
    /// 同时让 Cancel/丢弃能立刻中断在途请求，join 不再阻塞到 API 超时。
    AiHttpAbortSlot ownAbort_;
    bool lastTakeDiscarded_ = false;
    /// Prevent nested lookahead from lookahead itself
    bool inFlightPrefetch_ = false;
};
