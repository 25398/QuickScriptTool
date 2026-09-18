#include "ai_plan_util.h"

#include <algorithm>
#include <cmath>

namespace {

std::wstring FormatNum(double v) {
    if (std::fabs(v - std::llround(v)) < 0.05) return std::to_wstring(std::llround(v));
    wchar_t buf[32]{};
    swprintf_s(buf, L"%.2f", v);
    return buf;
}

int EffectiveMaxCount(const PlanSpendItem& it, bool distinctOnly) {
    if (distinctOnly) return 1;
    if (it.maxCount < 0) return 1000000;
    return it.maxCount;
}

}  // namespace

PlanSpendPlan PlanSpendBudget(double budget, int slotLimit, bool distinctOnly,
    const std::vector<PlanSpendItem>& items) {
    PlanSpendPlan plan;
    plan.budget = budget;

    // 只考虑：花费 >0、预算买得起、未被锁定的候选
    std::vector<size_t> idx;
    for (size_t i = 0; i < items.size(); ++i) {
        const PlanSpendItem& it = items[i];
        if (it.name.empty() || it.cost <= 0.0) continue;
        if (it.cost > budget) continue;
        if (EffectiveMaxCount(it, distinctOnly) <= 0) continue;
        idx.push_back(i);
    }
    // 性价比优先；同性价比时价值高的优先（贵而强的更稳）
    std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) {
        const PlanSpendItem& x = items[a];
        const PlanSpendItem& y = items[b];
        const double vx = x.value > 0.0 ? x.value : x.cost;
        const double vy = y.value > 0.0 ? y.value : y.cost;
        const double rx = vx / x.cost;
        const double ry = vy / y.cost;
        if (std::fabs(rx - ry) > 1e-9) return rx > ry;
        return vx > vy;
    });

    double remaining = budget;
    int slotsUsed = 0;
    for (size_t k : idx) {
        const PlanSpendItem& it = items[k];
        const double v = it.value > 0.0 ? it.value : it.cost;
        int canTake = 0;
        if (slotLimit > 0 && slotsUsed >= slotLimit) break;
        const int cap = EffectiveMaxCount(it, distinctOnly);
        canTake = static_cast<int>(std::floor(remaining / it.cost + 1e-9));
        if (canTake > cap) canTake = cap;
        if (slotLimit > 0 && slotsUsed + 1 > slotLimit) canTake = 0;
        if (canTake <= 0) continue;
        PlanSpendPick pick;
        pick.name = it.name;
        pick.count = canTake;
        pick.cost = it.cost * canTake;
        pick.value = v * canTake;
        remaining -= pick.cost;
        ++slotsUsed;
        plan.totalCount += canTake;
        plan.picks.push_back(std::move(pick));
    }
    plan.spent = budget - remaining;
    plan.remaining = remaining;

    std::wstring s;
    if (plan.picks.empty()) {
        s = L"预算 " + FormatNum(budget) + L" 买不起任何一项（最便宜的也超预算）。";
    } else {
        s = L"预算 " + FormatNum(budget) + L"：建议 ";
        for (size_t i = 0; i < plan.picks.size(); ++i) {
            if (i) s += L" + ";
            s += plan.picks[i].name + L"×" + std::to_wstring(plan.picks[i].count);
        }
        s += L"（共 " + std::to_wstring(plan.totalCount) + L" 个，花 "
            + FormatNum(plan.spent) + L"，剩 " + FormatNum(plan.remaining) + L"）。";
        if (distinctOnly)
            s += L"这是按「性价比优先」挑的卡；先按这个名单选卡，不要一键全选"
                 L"（弱卡会占满卡槽）。";
        else
            s += L"一次至少落这么多个，别只放一两个就停手。";
    }
    plan.summary = std::move(s);
    return plan;
}
