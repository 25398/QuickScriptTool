#include "ai_locate_cache.h"

#include "image_match.h"
#include "utils.h"

#include <algorithm>
#include <vector>

namespace {

struct CachedItem {
    AiLocateCacheKey key;
    AiLocateCacheEntry entry;
};

// 与 AiSession 同款：线程局部，AI 动作执行全程在同一条线程上
std::vector<CachedItem>& Cache() {
    static thread_local std::vector<CachedItem> items;
    return items;
}

constexpr size_t kMaxCacheEntries = 8;

bool SameKey(const AiLocateCacheKey& a, const AiLocateCacheKey& b) {
    return a.target == b.target
        && a.windowTitle == b.windowTitle
        && a.windowClass == b.windowClass
        && a.captureW == b.captureW
        && a.captureH == b.captureH;
}

}  // namespace

std::wstring AiLocateNormalizeTarget(const std::wstring& target) {
    std::wstring s = Trim(target);
    if (s.empty()) return s;
    // 通用控件后缀剥掉：写「保存按钮」和「保存」应命中同一条缓存
    static const wchar_t* kSuffixes[] = {
        L"按钮", L"图标", L"链接", L"菜单项", L"选项卡", L"输入框", L"搜索框", L"下拉框",
    };
    for (const wchar_t* suf : kSuffixes) {
        const size_t len = wcslen(suf);
        if (s.size() > len + 1 && s.compare(s.size() - len, len, suf) == 0) {
            s = Trim(s.substr(0, s.size() - len));
            break;
        }
    }
    // 折叠空白 + 转小写，避免「保存 」与「保存」被当成两条
    std::wstring out;
    out.reserve(s.size());
    bool sp = false;
    for (const wchar_t c : s) {
        if (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n') {
            if (!out.empty()) sp = true;
            continue;
        }
        if (sp) {
            out.push_back(L' ');
            sp = false;
        }
        out.push_back(c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c - L'A' + L'a') : c);
    }
    return Trim(out);
}

std::wstring AiLocateCacheKeyString(const AiLocateCacheKey& key) {
    return key.target + L"@" + key.windowTitle + L"#" + key.windowClass + L" "
        + std::to_wstring(key.captureW) + L"x" + std::to_wstring(key.captureH);
}

void AiLocateCacheStore(const AiLocateCacheKey& key, HBITMAP tmpl,
    int screenX, int screenY, int boxW, int boxH, const RECT* windowRect) {
    if (!tmpl) return;
    if (key.target.empty()) {
        DeleteBitmapHandle(tmpl);
        return;
    }
    const bool haveRect = windowRect != nullptr;
    const RECT rect = haveRect ? *windowRect : RECT{};
    auto& items = Cache();
    // 覆盖同键：先释放旧模板，避免位图泄漏
    for (auto& it : items) {
        if (!SameKey(it.key, key)) continue;
        if (it.entry.tmpl && it.entry.tmpl != tmpl) DeleteBitmapHandle(it.entry.tmpl);
        it.entry.tmpl = tmpl;
        it.entry.valid = true;
        it.entry.screenX = screenX;
        it.entry.screenY = screenY;
        it.entry.boxW = boxW;
        it.entry.boxH = boxH;
        it.entry.hits = 0;
        it.entry.windowRect = rect;
        it.entry.hasWindowRect = haveRect;
        return;
    }
    if (items.size() >= kMaxCacheEntries) {
        // 满了丢最旧的一条（按插入顺序），循环场景里旧目标也不该再占额度
        if (items.front().entry.tmpl) DeleteBitmapHandle(items.front().entry.tmpl);
        items.erase(items.begin());
    }
    CachedItem item;
    item.key = key;
    item.entry.tmpl = tmpl;
    item.entry.valid = true;
    item.entry.screenX = screenX;
    item.entry.screenY = screenY;
    item.entry.boxW = boxW;
    item.entry.boxH = boxH;
    item.entry.windowRect = rect;
    item.entry.hasWindowRect = haveRect;
    items.push_back(std::move(item));
}

bool AiLocateCacheLookup(const AiLocateCacheKey& key, AiLocateCacheEntry* out) {
    if (out) *out = AiLocateCacheEntry{};
    for (const auto& it : Cache()) {
        if (!SameKey(it.key, key)) continue;
        if (!it.entry.valid || !it.entry.tmpl) return false;
        if (out) *out = it.entry;
        return true;
    }
    return false;
}

void AiLocateCacheInvalidate(const AiLocateCacheKey& key) {
    auto& items = Cache();
    for (auto pos = items.begin(); pos != items.end(); ++pos) {
        if (!SameKey(pos->key, key)) continue;
        if (pos->entry.tmpl) DeleteBitmapHandle(pos->entry.tmpl);
        items.erase(pos);
        return;
    }
}

void AiLocateCacheNoteHit(const AiLocateCacheKey& key) {
    for (auto& it : Cache()) {
        if (SameKey(it.key, key)) {
            ++it.entry.hits;
            return;
        }
    }
}

void AiLocateCacheClear() {
    for (auto& it : Cache()) {
        if (it.entry.tmpl) DeleteBitmapHandle(it.entry.tmpl);
    }
    Cache().clear();
}

int AiLocateCacheSize() {
    return static_cast<int>(Cache().size());
}

bool ShouldAcceptAiLocateCacheHit(const AiLocateCacheAcceptInput& in,
    double minScore, double secondScoreGap, int secondDistGapPx) {
    if (in.bestScore < minScore) return false;
    const bool haveSecond = in.secondScore >= 0.0 && in.secondDistToCached >= 0;
    if (!haveSecond) return true;
    // 次佳同样高分且同样近 → 模板在这个界面上不唯一，宁可回识图
    const bool secondStrong = in.secondScore >= in.bestScore - secondScoreGap;
    const bool secondNear = in.secondDistToCached <= in.bestDistToCached + secondDistGapPx;
    return !(secondStrong && secondNear);
}

void AiLocateCacheReanchor(int storedX, int storedY, const RECT& storedRect,
    const RECT& currentRect, int* outX, int* outY) {
    const int dx = currentRect.left - storedRect.left;
    const int dy = currentRect.top - storedRect.top;
    if (outX) *outX = storedX + dx;
    if (outY) *outY = storedY + dy;
}

bool ShouldAcceptAiLocateCacheHitHysteresis(int passCount, int sampleCount,
    int needPass, int needSamples) {
    if (needSamples <= 0 || needPass <= 0) return false;
    if (sampleCount >= needSamples) return passCount >= needPass;
    // 采样不足（例如目标区已被别的窗口遮住/截屏失败）：按严格多数判，
    // 不允许「只采到一次就当真」——那正是单帧抖动误点的来源。
    return passCount >= needPass && sampleCount >= needPass;
}
