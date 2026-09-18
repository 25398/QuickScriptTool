#pragma once
//
// ai_plan_util.h — 「先算账」：预算 / 卡槽约束下的确定性资源分配
//
// 为什么要有这个：实测 AI 在资源分配上必然算错 ——
//   · 选卡时点「一键全选」，弱卡把卡槽占满，强卡进不来（阳光明明够）；
//   · 放僵尸时只放 2~3 只就停手（阳光够放几十只）。
// 这类「给定预算 + 槽位 + 一批单位，怎么花最划算」是有确定解的组合问题，
// 不该交给模型心算。业界同类做法：
//   · 预算/槽位约束的**贪心按性价比**分配（背包问题的经典近似，RTS/卡牌 AI 常用）；
//   · 智能体侧用**显式工具做算术**（plan-and-solve / tool-augmented reasoning）而不是让 LLM 心算。
// 本模块只做纯计算，模型给 items（名字/花费/强度/上限）与约束，拿回可照抄的方案。

#include <string>
#include <vector>

struct PlanSpendItem {
    std::wstring name;
    double cost = 0.0;      // 花费（阳光/金币/费用）
    double value = 0.0;     // 强度/价值；<=0 时用 cost 代替（贵的通常更强）
    int maxCount = -1;      // 最多能用几次；-1=不限（受预算约束）
    bool locked = false;    // 已选中/不可更换（例如已在卡槽里的）
};

struct PlanSpendPick {
    std::wstring name;
    int count = 0;
    double cost = 0.0;
    double value = 0.0;
};

struct PlanSpendPlan {
    std::vector<PlanSpendPick> picks;
    double budget = 0.0;
    double spent = 0.0;
    double remaining = 0.0;
    int totalCount = 0;
    /// 给模型看的一行结论（中文，可直接照做）
    std::wstring summary;
};

/// budget：总预算（阳光/金币）。
/// slotLimit：最多挑几种（卡槽数）；<=0 表示不限种类。
/// distinctOnly=true：每种最多 1 个（选卡场景）；false：可按预算重复放（放单位场景）。
/// 贪心按 value/cost 性价比排序，预算内尽量多拿；同价时价值高的先拿。
PlanSpendPlan PlanSpendBudget(double budget, int slotLimit, bool distinctOnly,
    const std::vector<PlanSpendItem>& items);
