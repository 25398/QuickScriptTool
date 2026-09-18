#pragma once
// 录制优化：鼠标移动合并 / 路径压缩（产品对话框与 AI 助手共用）
// 关键动作 = 除绝对/相对移动与等待外；选中含关键动作时以其为分割点分段处理。

#include "script_types.h"

#include <string>
#include <vector>

namespace recopt {

bool IsMoveOrWait(const ScriptAction& a);

struct IndexRange {
    int first = 0;
    int last = 0;
};

struct OptimizeApplyResult {
    bool collectOk = true;
    std::string collectErr;
    int applied = 0;
    int skippedRelative = 0;
    int skippedNoMove = 0;
    int skippedTooFew = 0;
};

/// 未选关键动作：整段必须连续且全是移动/等待。
/// 选中了关键动作：按已选下标切段，关键动作只作分割点；未勾选空洞也分段。
bool CollectMoveWaitRanges(const std::vector<ScriptAction>& actions,
    const std::vector<char>& selected, bool forMerge,
    std::vector<IndexRange>& ranges, std::string& err);

/// waitCalc：sum / average|avg / first / last / fixed|custom|specified
OptimizeApplyResult MergeSelected(std::vector<ScriptAction>& actions,
    const std::vector<char>& selected, const std::string& waitCalc, double fixedWait);

OptimizeApplyResult CompressSelected(std::vector<ScriptAction>& actions,
    const std::vector<char>& selected, double distanceThreshold,
    const std::string& waitCalc, double fixedWait);

/// 整文件：相当于全选（含关键动作），与产品「跨关键动作合并/压缩」一致。
OptimizeApplyResult MergeAllKeySplit(std::vector<ScriptAction>& actions,
    const std::string& waitCalc, double fixedWait);

OptimizeApplyResult CompressAllKeySplit(std::vector<ScriptAction>& actions,
    double distanceThreshold, const std::string& waitCalc, double fixedWait);

}  // namespace recopt
