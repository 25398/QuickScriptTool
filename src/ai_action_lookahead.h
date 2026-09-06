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
    std::wstring TakeHintAfterObserve(
        bool observeUnchanged,
        bool onlyDynamicChanged,
        bool dialogBlocking,
        int waitMs = 6000);

    /// For SKIP_OBSERVE rounds: take hint if ready (no UI surprise assumed).
    std::wstring TakeHintSkipObserve(int waitMs = 4000);

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
    /// Prevent nested lookahead from lookahead itself
    bool inFlightPrefetch_ = false;
};
