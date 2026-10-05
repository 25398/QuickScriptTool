// =============================================================================
// TimeScaleSelfTest — 窗口变速（变速齿轮）时钟换算
// =============================================================================
// 总索引：.cursor/skills/module-selftest/SKILL.md
//   MSBuild ... /t:TimeScaleSelfTest
//   build\Release\TimeScaleSelfTest.exe --json
//
// 为什么有这个 suite：
//   变速的实现分两半 —— 一半是目标进程里的 IAT 补丁（必须有目标进程才能验，
//   归 WindowModeSelfTest 的交互档），另一半是**虚拟时钟的整数换算**
//   （src/window_mode/time_scale_clock.h）。后者最容易出三类错，而且全是静默的：
//     ① 换倍率时时间跳变/倒流 → 游戏里表现为瞬移、计时器错乱；
//     ② 溢出 → 时钟突然归零或倒着走；
//     ③ 几个时钟（QPC / tick / FILETIME）互相漂移 → 同一份游戏时间在两条路径上对不上。
//   这三类都能在纯逻辑层直接断言，所以本 suite 覆盖它们；IAT 补丁本身
//   （补到哪个槽、卸载是否写回）由 FakeFocus 的 FakeFocus_TimeScaleDiag 暴露，
//   在 WindowModeSelfTest 的交互档里验。
// =============================================================================
#include "selftest_harness.h"

#include "window_mode/time_scale_clock.h"

#include <cmath>
#include <cstdint>
#include <string>

namespace {

using fakefocus::kTimeScaleDen;
using fakefocus::ScaleElapsed;
using fakefocus::TimeScaleAnchors;
using fakefocus::TimeScaleFromDouble;
using fakefocus::TimeScaleIsIdentity;
using selftest::Emit;

constexpr uint64_t kFreq = 10000000ull;  // 10MHz，QPC 常见值

const selftest::CaseInfo kCases[] = {
    {L"clamp_from_double", L"default",
        L"倍率夹取：1.0→1000、2.0→2000、0.25→250、超界夹到 100~16000；0/负数/NaN 回原速"},
    {L"identity_detection", L"default",
        L"0 与 1000 都算原速（0=关闭，1000=挂钩但原样转发）"},
    {L"scale_elapsed_math", L"default",
        L"delta*num/den：2 倍速下 1 秒真实时间走 2 秒虚拟时间；1 倍速恒等"},
    {L"rebase_no_jump", L"default",
        L"换倍率（1→2→1→0.5）时虚拟时钟连续：前后两次读取的差值不跳变、不倒流"},
    {L"rebase_no_backward", L"default",
        L"锚点重设后虚拟时钟单调不减（含时间源回退的极端输入）"},
    {L"clock_consistency", L"default",
        L"QPC / tick / FILETIME 三条时钟同源：同一虚拟时刻换算出的毫秒与 100ns 数一致"},
    {L"no_overflow_long_run", L"default",
        L"连续 6 小时不重设锚点也不溢出（饱和保护生效且不归零）"},
    {L"identity_is_exact", L"default",
        L"倍率 1000 时虚拟值 == 真实值（原速必须逐位一致，否则关闭功能仍有副作用）"},
};

void CaseClampFromDouble() {
    struct Row {
        double in;
        uint32_t want;
        const wchar_t* what;
    };
    const Row rows[] = {
        {1.0, 1000, L"1.0"},
        {2.0, 2000, L"2.0"},
        {4.0, 4000, L"4.0"},
        {0.25, 250, L"0.25"},
        {0.1, 100, L"0.1（下界）"},
        {0.01, 100, L"0.01 夹到下界"},
        {100.0, 16000, L"100 夹到上界"},
        {0.0, 1000, L"0 回原速"},
        {-1.0, 1000, L"负数回原速"},
    };
    for (const Row& r : rows) {
        const uint32_t got = TimeScaleFromDouble(r.in);
        if (got != r.want) {
            Emit(L"clamp_from_double", false,
                (std::wstring(L"倍率 ") + r.what + L" 期望 " + std::to_wstring(r.want)
                    + L" 实际 " + std::to_wstring(got))
                    .c_str());
            return;
        }
    }
    const double nan = std::nan("");
    if (TimeScaleFromDouble(nan) != 1000) {
        Emit(L"clamp_from_double", false, L"NaN 应回原速 1000");
        return;
    }
    Emit(L"clamp_from_double", true, L"9 组边界 + NaN 全部符合");
}

void CaseIdentityDetection() {
    const bool ok = TimeScaleIsIdentity(0) && TimeScaleIsIdentity(1000)
        && !TimeScaleIsIdentity(1001) && !TimeScaleIsIdentity(999)
        && !TimeScaleIsIdentity(2000);
    Emit(L"identity_detection", ok, ok ? L"0/1000 为原速，其余不是" : L"原速判定错误");
}

void CaseScaleElapsedMath() {
    // 1 秒真实（10MHz → 1e7 计数）在 2 倍速下应走 2e7 计数。
    const uint64_t oneSec = kFreq;
    const uint64_t x2 = ScaleElapsed(oneSec, 2000, kTimeScaleDen);
    const uint64_t x05 = ScaleElapsed(oneSec, 500, kTimeScaleDen);
    const uint64_t x1 = ScaleElapsed(oneSec, 1000, kTimeScaleDen);
    const bool ok = x2 == 2 * oneSec && x05 == oneSec / 2 && x1 == oneSec;
    Emit(L"scale_elapsed_math", ok,
        ok ? L"2x/1x/0.5x 换算正确"
           : (L"期望 20000000/10000000/5000000，实际 " + std::to_wstring(x2) + L"/"
                 + std::to_wstring(x1) + L"/" + std::to_wstring(x05))
                 .c_str());
}

void CaseRebaseNoJump() {
    TimeScaleAnchors a{};
    uint64_t realQpc = 1000000000ull;
    uint64_t realTick = 500000ull;
    uint64_t realFt = 133000000000000000ull;
    fakefocus::InitAnchors(a, realQpc, realTick, realFt);

    // 每步推进 100ms 真实时间，中途换倍率，检查虚拟时钟连续且单调。
    const uint64_t step = kFreq / 10;  // 100ms
    const uint32_t scales[] = {1000, 2000, 2000, 1000, 500, 4000, 4000, 250};
    uint64_t prevVirt = fakefocus::VirtualQpc(a, realQpc);
    for (size_t i = 0; i < sizeof(scales) / sizeof(scales[0]); ++i) {
        realQpc += step;
        realTick += 100;
        realFt += 1000000ull;  // 100ms = 1e6 * 100ns
        fakefocus::RebaseAnchors(a, realQpc, realTick, realFt, scales[i], kFreq);
        const uint64_t virt = fakefocus::VirtualQpc(a, realQpc);
        if (virt < prevVirt) {
            Emit(L"rebase_no_jump", false,
                (L"第 " + std::to_wstring(i) + L" 步换倍率后虚拟时钟倒流："
                    + std::to_wstring(prevVirt) + L" → " + std::to_wstring(virt))
                    .c_str());
            return;
        }
        // 单步虚拟增量不应超过「最大倍率 × 真实增量」，否则说明锚点重设把时间吃了或重复计。
        const uint64_t delta = virt - prevVirt;
        if (delta > step * 16) {
            Emit(L"rebase_no_jump", false,
                (L"第 " + std::to_wstring(i) + L" 步虚拟增量异常：" + std::to_wstring(delta))
                    .c_str());
            return;
        }
        prevVirt = virt;
    }
    Emit(L"rebase_no_jump", true, L"8 次换倍率（含 4x / 0.25x）虚拟时钟单调且无跳变");
}

void CaseRebaseNoBackward() {
    TimeScaleAnchors a{};
    fakefocus::InitAnchors(a, 500000ull, 1000ull, 133000000000000000ull);
    // 真实时间源回退（QPC 理论单调，但硬件/虚拟化下偶见）不得让虚拟时钟倒流。
    fakefocus::RebaseAnchors(a, 400000ull, 900ull, 133000000000000000ull, 2000, kFreq);
    const uint64_t v1 = fakefocus::VirtualQpc(a, 400000ull);
    fakefocus::RebaseAnchors(a, 600000ull, 1000ull, 133000000000000000ull, 2000, kFreq);
    const uint64_t v2 = fakefocus::VirtualQpc(a, 600000ull);
    const bool ok = v2 >= v1;
    Emit(L"rebase_no_backward", ok,
        ok ? L"时间源回退时虚拟时钟不倒流"
           : (L"倒流：" + std::to_wstring(v1) + L" → " + std::to_wstring(v2)).c_str());
}

void CaseClockConsistency() {
    TimeScaleAnchors a{};
    const uint64_t realTick0 = 123456ull;
    const uint64_t realFt0 = 133000000000000000ull;
    fakefocus::InitAnchors(a, 0ull, realTick0, realFt0);
    // 在 t=0 处切到 3 倍速，然后走 1 秒真实时间。
    fakefocus::RebaseAnchors(a, 0ull, realTick0, realFt0, 3000, kFreq);
    const uint64_t real = kFreq;  // 1 秒真实
    const uint64_t virt = fakefocus::VirtualQpc(a, real);
    const uint64_t tickMs = fakefocus::VirtualTickMs(a, virt, kFreq);
    const uint64_t ft = fakefocus::VirtualFileTime(a, virt, kFreq);

    const bool qpcOk = virt == 3 * kFreq;                  // 1 秒真实 → 3 秒虚拟
    const bool tickOk = tickMs == realTick0 + 3000;         // → tick 走 3000ms
    const bool ftOk = ft == realFt0 + 30000000ull;          // → FILETIME 走 3e7 × 100ns
    const bool ok = qpcOk && tickOk && ftOk;
    Emit(L"clock_consistency", ok,
        ok ? L"1 秒真实 @3x → QPC +3s、tick +3000ms、FILETIME +3e7（100ns）三条时钟一致"
           : (L"qpc=" + std::to_wstring(virt) + L"（期望 " + std::to_wstring(3 * kFreq)
                 + L"），tick=" + std::to_wstring(tickMs) + L"（期望 "
                 + std::to_wstring(realTick0 + 3000) + L"），ft=" + std::to_wstring(ft)
                 + L"（期望 " + std::to_wstring(realFt0 + 30000000ull) + L"）")
                 .c_str());
}

void CaseNoOverflowLongRun() {
    TimeScaleAnchors a{};
    const uint64_t ftBase = 133000000000000000ull;
    fakefocus::InitAnchors(a, 0ull, 0ull, ftBase);
    // 6 小时不重设锚点：10MHz → 2.16e11 计数；4 倍速 → 8.64e11，仍在 64 位内。
    // 这里把「饱和保护」也算进来：无论多久都不得归零/回绕。
    const uint64_t sixHours = 6ull * 3600ull * kFreq;
    fakefocus::RebaseAnchors(a, sixHours, 6ull * 3600ull * 1000ull, ftBase, 4000, kFreq);
    const uint64_t virt = fakefocus::VirtualQpc(a, sixHours);
    const uint64_t tick = fakefocus::VirtualTickMs(a, virt, kFreq);
    const uint64_t ft = fakefocus::VirtualFileTime(a, virt, kFreq);
    // 顺带把「饱和保护」分支逼出来：delta 远超 2^40 时不得溢出回绕成小值/零。
    const uint64_t sat = ScaleElapsed(1ull << 50, 16000, kTimeScaleDen);
    const bool satOk = sat == (1ull << 40) * 16ull;
    const bool ok = virt == sixHours && tick == 6ull * 3600ull * 1000ull && ft == ftBase
        && satOk;
    Emit(L"no_overflow_long_run", ok,
        ok ? L"6 小时虚拟时钟未溢出（tick 精确到毫秒）；饱和分支 2^50→2^40 倍乘不溢出"
           : (L"virt=" + std::to_wstring(virt) + L" tick=" + std::to_wstring(tick)
                 + L" ft=" + std::to_wstring(ft) + L" sat=" + std::to_wstring(sat)
                 + L"（期望 " + std::to_wstring((1ull << 40) * 16ull) + L"）")
                 .c_str());
}

void CaseIdentityIsExact() {
    TimeScaleAnchors a{};
    fakefocus::InitAnchors(a, 777777ull, 111ull, 222ull);
    bool ok = true;
    uint64_t real = 777777ull;
    for (int i = 0; i < 64; ++i) {
        real += 1234567ull;
        fakefocus::RebaseAnchors(a, real, 111ull + i, 222ull + i, kTimeScaleDen, kFreq);
        if (fakefocus::VirtualQpc(a, real) != real) {
            ok = false;
            break;
        }
    }
    Emit(L"identity_is_exact", ok,
        ok ? L"倍率 1000 时虚拟 QPC 与真实值逐位相同（64 次抽样）"
           : L"原速下虚拟值与真实值不等 —— 关闭功能也会有副作用");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    bool listOnly = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i] ? argv[i] : L"";
        if (a == L"--json") {
            selftest::gJson = true;
            selftest::InitUtf8Stdout();
        } else if (a == L"--list") {
            listOnly = true;
            selftest::InitUtf8Stdout();
        } else if (a == L"--help" || a == L"-h") {
            std::fwprintf(stdout,
                L"TimeScaleSelfTest — 窗口变速时钟换算\n"
                L"  --json  逐行 JSON\n  --list  只列用例\n");
            return 0;
        }
    }
    if (listOnly) {
        selftest::PrintCaseList(L"TimeScaleSelfTest", kCases,
            sizeof(kCases) / sizeof(kCases[0]));
        return 0;
    }

    CaseClampFromDouble();
    CaseIdentityDetection();
    CaseScaleElapsedMath();
    CaseRebaseNoJump();
    CaseRebaseNoBackward();
    CaseClockConsistency();
    CaseNoOverflowLongRun();
    CaseIdentityIsExact();

    selftest::EmitSummary();
    return selftest::ExitCode();
}
