#include "ai_ui_layout.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <vector>

// ★本文件在 SCRIPT_CORE_COMMON（轻量库）里，**不许**依赖 process_utils 等上层工具库：
//   一旦依赖，所有只链 script_core_common 的自检目标都会 LNK2019（已踩：
//   ImageMatchSelfTest / WindowModeSelfTest / ScriptActionBuilderSelfTest 全线链接失败，
//   而旧 exe 还在，于是「自检全绿」是跑在过期二进制上的假象）。
//   这里只需要「进程 exe 名」，就地取一次即可。
namespace {

std::wstring LocalProcessImageName(DWORD pid) {
    if (!pid) return {};
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return {};
    wchar_t path[MAX_PATH]{};
    DWORD size = MAX_PATH;
    const BOOL ok = QueryFullProcessImageNameW(process, 0, path, &size);
    CloseHandle(process);
    if (!ok) return {};
    const wchar_t* slash = wcsrchr(path, L'\\');
    return slash ? std::wstring(slash + 1) : std::wstring(path);
}

}  // namespace

namespace {

struct Entry {
    AiUiLayoutRect rect;
    AiUiGridSpec grid;
    bool hasGrid = false;
    uint64_t lastUse = 0;
    /// 命中次数（用得越多越可信）
    int hits = 0;
    /// **连续**「照记忆点下去没变化」的次数。单次不算数：小范围颜色采样在
    /// 「工具本身耗时几秒后画面早就变了」这类情况下会误报（实测每次点击都误报）。
    /// 连续 2 次才判为「这条记忆过期」，此时**不删除**，只让下一次走真识图刷新它 ——
    /// 用户的意见：算出来的坐标不总是准，界面没大变化就该留着并继续积累数据。
    int misses = 0;
    int samples = 0;
    /// 外观签名（记住坐标那一刻的格子平均亮度）——命中前硬校验用
    uint8_t sig[kAiUiSigN * kAiUiSigN]{};
    bool hasSig = false;
};

std::mutex g_mu;
std::unordered_map<std::wstring, Entry> g_entries;
uint64_t g_stamp = 0;
int g_hits = 0;
int g_misses = 0;

constexpr size_t kMaxEntries = 128;

std::wstring MakeKey(const AiUiLayoutKey& key) {
    // 用不可见分隔符拼键，避免 target 里含 '|' 时串键
    std::wstring s = key.target;
    s += L'\x1f';
    s += key.windowIdentity;
    s += L'\x1f';
    s += std::to_wstring(key.screenW);
    s += L'x';
    s += std::to_wstring(key.screenH);
    return s;
}

/// 自位移平均绝对差：d(lag) = mean |row(x) − row(x+lag)|，取显著极小值当周期。
/// 网格（草坪/卡槽/图标阵列）在 lag = 周期处几乎完全重合 → d 出现深谷；
/// 不规则画面没有这种稳定的谷 → 返回 false。
bool DetectPeriodAlongX(const uint8_t* gray, int width, int height, int stride,
    const AiUiLayoutRect& band, int minPeriod, int maxPeriod, int& periodOut) {
    periodOut = 0;
    // X 方向周期：**行**带取锚点那几行（那是网格所在的行），列方向铺满全宽
    const int y1 = std::max(0, band.y1);
    const int y2 = std::min(height, band.y2);
    const int x1 = 0;
    const int x2 = width;
    if (x2 - x1 < 8 || y2 - y1 < 4) return false;
    const int maxLag = std::min(maxPeriod, (x2 - x1) - 4);
    if (maxLag < minPeriod) return false;

    std::vector<double> diffs(static_cast<size_t>(maxLag) + 1, -1.0);
    double best = 1e18;
    int bestLag = 0;
    double sum = 0.0;
    int count = 0;
    for (int lag = minPeriod; lag <= maxLag; ++lag) {
        double acc = 0.0;
        int n = 0;
        for (int y = y1; y < y2; ++y) {
            const uint8_t* row = gray + static_cast<size_t>(y) * stride;
            for (int x = x1; x + lag < x2; ++x) {
                acc += std::fabs(static_cast<double>(row[x]) - static_cast<double>(row[x + lag]));
                ++n;
            }
        }
        if (n == 0) return false;
        const double d = acc / n;
        diffs[static_cast<size_t>(lag)] = d;
        sum += d;
        ++count;
        if (d < best) {
            best = d;
            bestLag = lag;
        }
    }
    if (count == 0 || bestLag == 0) return false;
    const double mean = sum / count;
    // ★三道闸，缺一不可（实测踩过：暂停菜单上误报 16×16px 周期，会让模型去点 79 列）：
    //   ① 谷底要显著低于平均；② 像素差本身要小；③ **周期不能远小于元素自身**
    //   （网格间距总不会比一个格子小很多），这条直接干掉纹理/扫描线造成的假周期。
    if (!(best < mean * 0.62) || best > 26.0) return false;
    // ★网格间距不会远小于元素自身：锚点框在宿主里是「定位点 ±24px」（48px），
    // 真网格（卡槽/草坪格/图标阵列）间距都 ≥32px；实测暂停菜单的石纹背景会误报
    // 16px 周期 → 推出「79 列」这种荒唐网格，照它点就是乱点。这条直接否掉。
    const int minPitch = (std::max)(28, (std::min)(band.width(), band.height()) * 3 / 5);
    if (bestLag < minPitch) return false;
    // 亚像素级：在谷底附近取抛物线顶点，避免周期取整误差累积（10 格后偏 10px）
    double refined = static_cast<double>(bestLag);
    if (bestLag > minPeriod && bestLag < maxLag) {
        const double d0 = diffs[static_cast<size_t>(bestLag - 1)];
        const double d1 = best;
        const double d2 = diffs[static_cast<size_t>(bestLag + 1)];
        if (d0 >= 0.0 && d2 >= 0.0) {
            const double denom = (d0 - 2.0 * d1 + d2);
            if (std::fabs(denom) > 1e-9) refined += 0.5 * (d0 - d2) / denom;
        }
    }
    periodOut = static_cast<int>(std::lround(refined));
    return periodOut >= minPeriod && periodOut <= maxPeriod;
}

bool DetectPeriodAlongY(const uint8_t* gray, int width, int height, int stride,
    const AiUiLayoutRect& band, int minPeriod, int maxPeriod, int& periodOut) {
    periodOut = 0;
    // Y 方向周期：**列**带取锚点那几列，行方向铺满全高
    const int y1 = 0;
    const int y2 = height;
    const int x1 = std::max(0, band.x1);
    const int x2 = std::min(width, band.x2);
    if (y2 - y1 < 8 || x2 - x1 < 4) return false;
    const int maxLag = std::min(maxPeriod, (y2 - y1) - 4);
    if (maxLag < minPeriod) return false;

    std::vector<double> diffs(static_cast<size_t>(maxLag) + 1, -1.0);
    double best = 1e18;
    int bestLag = 0;
    double sum = 0.0;
    int count = 0;
    for (int lag = minPeriod; lag <= maxLag; ++lag) {
        double acc = 0.0;
        int n = 0;
        for (int y = y1; y + lag < y2; ++y) {
            const uint8_t* rowA = gray + static_cast<size_t>(y) * stride;
            const uint8_t* rowB = gray + static_cast<size_t>(y + lag) * stride;
            for (int x = x1; x < x2; ++x) {
                acc += std::fabs(static_cast<double>(rowA[x]) - static_cast<double>(rowB[x]));
                ++n;
            }
        }
        if (n == 0) return false;
        const double d = acc / n;
        diffs[static_cast<size_t>(lag)] = d;
        sum += d;
        ++count;
        if (d < best) {
            best = d;
            bestLag = lag;
        }
    }
    if (count == 0 || bestLag == 0) return false;
    const double mean = sum / count;
    if (!(best < mean * 0.62) || best > 26.0) return false;
    const int minPitch = (std::max)(28, (std::min)(band.width(), band.height()) * 3 / 5);
    if (bestLag < minPitch) return false;
    double refined = static_cast<double>(bestLag);
    if (bestLag > minPeriod && bestLag < maxLag) {
        const double d0 = diffs[static_cast<size_t>(bestLag - 1)];
        const double d1 = best;
        const double d2 = diffs[static_cast<size_t>(bestLag + 1)];
        if (d0 >= 0.0 && d2 >= 0.0) {
            const double denom = (d0 - 2.0 * d1 + d2);
            if (std::fabs(denom) > 1e-9) refined += 0.5 * (d0 - d2) / denom;
        }
    }
    periodOut = static_cast<int>(std::lround(refined));
    return periodOut >= minPeriod && periodOut <= maxPeriod;
}

}  // namespace

bool AiUiGridCellCenter(const AiUiGridSpec& grid, int row, int col, int& x, int& y) {
    if (!grid.valid()) return false;
    // 允许负偏移（锚点左边的卡槽、上面的草坪行）；但要挡住离谱的越界值
    if (col < -64 || col > 64 || row < -64 || row > 64) return false;
    x = grid.originX + col * grid.stepX;
    y = grid.originY + row * grid.stepY;
    return true;
}

bool AiUiDetectGridPeriod(const uint8_t* gray, int width, int height, int stride,
    const AiUiLayoutRect& anchor, int minPeriod, int maxPeriod,
    int& periodX, int& periodY) {
    periodX = 0;
    periodY = 0;
    if (!gray || width <= 0 || height <= 0 || stride < width || !anchor.valid()) return false;
    if (minPeriod < 4 || maxPeriod <= minPeriod) return false;
    // 在锚点所在行/列带上找周期：X 用锚点那一行带（高度就是锚点高），
    // Y 用锚点那一列带。这样不会被别的行/列干扰。
    bool any = false;
    int px = 0;
    if (DetectPeriodAlongX(gray, width, height, stride, anchor, minPeriod, maxPeriod, px)) {
        periodX = px;
        any = true;
    }
    int py = 0;
    if (DetectPeriodAlongY(gray, width, height, stride, anchor, minPeriod, maxPeriod, py)) {
        periodY = py;
        any = true;
    }
    return any;
}

bool AiUiBitmapToGrayBytes(HBITMAP bmp, std::vector<uint8_t>& out, int& width, int& height,
    int& stride) {
    out.clear();
    width = height = stride = 0;
    if (!bmp) return false;
    BITMAP bm{};
    if (!GetObjectW(bmp, sizeof(bm), &bm) || bm.bmWidth <= 0 || bm.bmHeight <= 0) return false;
    const int w = bm.bmWidth;
    const int h = bm.bmHeight;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;   // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    std::vector<uint8_t> bgra(static_cast<size_t>(w) * h * 4);
    HDC dc = GetDC(nullptr);
    if (!dc) return false;
    const int lines = GetDIBits(dc, bmp, 0, h, bgra.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    if (lines <= 0) return false;
    width = w;
    height = h;
    stride = w;
    out.resize(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; ++y) {
        const uint8_t* src = bgra.data() + static_cast<size_t>(y) * w * 4;
        uint8_t* dst = out.data() + static_cast<size_t>(y) * w;
        for (int x = 0; x < w; ++x) {
            // Rec.601 亮度（整数近似）：BGR 顺序
            const int b = src[x * 4 + 0];
            const int g = src[x * 4 + 1];
            const int r = src[x * 4 + 2];
            dst[x] = static_cast<uint8_t>((r * 77 + g * 150 + b * 29) >> 8);
        }
    }
    return true;
}

bool AiUiLayoutSignature(const uint8_t* gray, int width, int height, int stride,
    const AiUiLayoutRect& rect, uint8_t outSig[kAiUiSigN * kAiUiSigN]) {
    if (!gray || !rect.valid()) return false;
    // 外扩 20%（至少 6px）：只比对目标中心容易漏掉「整体换了内容」的情况
    const int mw = (std::max)(6, rect.width() / 5);
    const int mh = (std::max)(6, rect.height() / 5);
    const int x1 = (std::max)(0, rect.x1 - mw);
    const int y1 = (std::max)(0, rect.y1 - mh);
    const int x2 = (std::min)(width, rect.x2 + mw);
    const int y2 = (std::min)(height, rect.y2 + mh);
    if (x2 - x1 < kAiUiSigN || y2 - y1 < kAiUiSigN) return false;
    for (int gy = 0; gy < kAiUiSigN; ++gy) {
        for (int gx = 0; gx < kAiUiSigN; ++gx) {
            const int sx1 = x1 + (x2 - x1) * gx / kAiUiSigN;
            const int sx2 = (std::max)(sx1 + 1, x1 + (x2 - x1) * (gx + 1) / kAiUiSigN);
            const int sy1 = y1 + (y2 - y1) * gy / kAiUiSigN;
            const int sy2 = (std::max)(sy1 + 1, y1 + (y2 - y1) * (gy + 1) / kAiUiSigN);
            long long acc = 0;
            int n = 0;
            for (int y = sy1; y < sy2 && y < height; ++y) {
                const uint8_t* row = gray + static_cast<size_t>(y) * stride;
                for (int x = sx1; x < sx2 && x < width; ++x) {
                    acc += row[x];
                    ++n;
                }
            }
            outSig[gy * kAiUiSigN + gx] = n > 0
                ? static_cast<uint8_t>(acc / n) : 0;
        }
    }
    return true;
}

void AiUiLayoutRememberSignature(const AiUiLayoutKey& key,
    const uint8_t sig[kAiUiSigN * kAiUiSigN]) {
    if (key.empty() || !sig) return;
    std::lock_guard<std::mutex> lock(g_mu);
    const auto it = g_entries.find(MakeKey(key));
    if (it == g_entries.end()) return;
    for (int i = 0; i < kAiUiSigN * kAiUiSigN; ++i) it->second.sig[i] = sig[i];
    it->second.hasSig = true;
}

bool AiUiLayoutSignatureMatches(const AiUiLayoutKey& key,
    const uint8_t sig[kAiUiSigN * kAiUiSigN], int tol) {
    if (key.empty() || !sig) return false;
    std::lock_guard<std::mutex> lock(g_mu);
    const auto it = g_entries.find(MakeKey(key));
    if (it == g_entries.end()) return false;
    if (!it->second.hasSig) return true;   // 没签名（旧条目/未记录）→ 不拦，沿用原行为
    long long acc = 0;
    for (int i = 0; i < kAiUiSigN * kAiUiSigN; ++i) {
        acc += std::abs(static_cast<int>(it->second.sig[i]) - static_cast<int>(sig[i]));
    }
    return (acc / (kAiUiSigN * kAiUiSigN)) <= tol;
}

void AiUiLayoutRemember(const AiUiLayoutKey& key, const AiUiLayoutRect& rect) {
    if (key.empty() || !rect.valid()) return;
    std::lock_guard<std::mutex> lock(g_mu);
    auto it = g_entries.find(MakeKey(key));
    if (it == g_entries.end()) {
        Entry e;
        e.rect = rect;
        e.lastUse = ++g_stamp;
        e.hits = 1;
        e.samples = 1;
        g_entries.emplace(MakeKey(key), std::move(e));
    } else {
        Entry& e = it->second;
        // ★多次定位取平均，让坐标越用越准（用户要求：用更多数据构建界面模型）。
        // 偏差很大（>24px）说明界面确实动过 → 直接换新值、清空历史。
        const int dx = std::abs(rect.cx() - e.rect.cx());
        const int dy = std::abs(rect.cy() - e.rect.cy());
        if (dx > 24 || dy > 24) {
            e.rect = rect;
            e.samples = 1;
        } else {
            const int n = e.samples;
            e.rect.x1 = (e.rect.x1 * n + rect.x1) / (n + 1);
            e.rect.y1 = (e.rect.y1 * n + rect.y1) / (n + 1);
            e.rect.x2 = (e.rect.x2 * n + rect.x2) / (n + 1);
            e.rect.y2 = (e.rect.y2 * n + rect.y2) / (n + 1);
            if (e.samples < 64) ++e.samples;
        }
        ++e.hits;
        e.misses = 0;
        e.lastUse = ++g_stamp;
    }
    if (g_entries.size() > kMaxEntries) {
        // 简单淘汰：丢掉最久没用过的一半（布局条目很少，不值得上 LRU 链表）
        std::vector<std::pair<uint64_t, std::wstring>> all;
        all.reserve(g_entries.size());
        for (const auto& kv : g_entries) all.emplace_back(kv.second.lastUse, kv.first);
        std::sort(all.begin(), all.end());
        const size_t drop = all.size() / 2;
        for (size_t i = 0; i < drop; ++i) g_entries.erase(all[i].second);
    }
}

bool AiUiLayoutRecall(const AiUiLayoutKey& key, AiUiLayoutRect& out) {
    if (key.empty()) return false;
    std::lock_guard<std::mutex> lock(g_mu);
    const auto it = g_entries.find(MakeKey(key));
    if (it == g_entries.end() || !it->second.rect.valid()) {
        ++g_misses;
        return false;
    }
    // 连续 2 次「照记忆点没反应」→ 这条记忆过期：本次回退真识图（条目留着，
    // 下次 Remember 会刷新它），而不是删掉。
    if (it->second.misses >= 2) {
        ++g_misses;
        return false;
    }
    it->second.lastUse = ++g_stamp;
    out = it->second.rect;
    ++g_hits;
    return true;
}

void AiUiLayoutNoteClickNoEffect(const AiUiLayoutKey& key) {
    if (key.empty()) return;
    std::lock_guard<std::mutex> lock(g_mu);
    const auto it = g_entries.find(MakeKey(key));
    if (it == g_entries.end()) return;
    // 只是「记一笔」，不删；连续两次才让 Recall 回退真识图
    if (it->second.misses < 1000) ++it->second.misses;
    // 网格坐标来自周期推断，可能略偏：同步记录一次 miss，够了就重新推断
    if (it->second.misses >= 2) it->second.hasGrid = false;
}

int AiUiLayoutStaleCount() {
    std::lock_guard<std::mutex> lock(g_mu);
    int n = 0;
    for (const auto& kv : g_entries) {
        if (kv.second.misses >= 2) ++n;
    }
    return n;
}

void AiUiLayoutRememberGrid(const AiUiLayoutKey& key, const AiUiGridSpec& grid) {
    if (key.empty() || !grid.valid()) return;
    std::lock_guard<std::mutex> lock(g_mu);
    Entry& e = g_entries[MakeKey(key)];
    e.grid = grid;
    e.hasGrid = true;
    e.lastUse = ++g_stamp;
}

bool AiUiLayoutRecallGrid(const AiUiLayoutKey& key, AiUiGridSpec& out) {
    if (key.empty()) return false;
    std::lock_guard<std::mutex> lock(g_mu);
    const auto it = g_entries.find(MakeKey(key));
    if (it == g_entries.end() || !it->second.hasGrid || !it->second.grid.valid()) return false;
    it->second.lastUse = ++g_stamp;
    out = it->second.grid;
    return true;
}

void AiUiLayoutForget(const AiUiLayoutKey& key) {
    if (key.empty()) return;
    std::lock_guard<std::mutex> lock(g_mu);
    g_entries.erase(MakeKey(key));
}

void AiUiLayoutClear() {
    std::lock_guard<std::mutex> lock(g_mu);
    g_entries.clear();
    g_stamp = 0;
    g_hits = 0;
    g_misses = 0;
}

int AiUiLayoutCount() {
    std::lock_guard<std::mutex> lock(g_mu);
    return static_cast<int>(g_entries.size());
}

int AiUiLayoutHitCount() {
    std::lock_guard<std::mutex> lock(g_mu);
    return g_hits;
}

int AiUiLayoutMissCount() {
    std::lock_guard<std::mutex> lock(g_mu);
    return g_misses;
}

std::wstring AiUiWindowIdentityForLayout(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return {};
    wchar_t title[512]{};
    GetWindowTextW(hwnd, title, 512);
    wchar_t cls[128]{};
    GetClassNameW(hwnd, cls, 128);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    std::wstring proc;
    if (pid) proc = LocalProcessImageName(pid);
    RECT rc{};
    GetClientRect(hwnd, &rc);
    std::wstring id = title;
    id += L'|';
    id += cls;
    id += L'|';
    id += proc;
    id += L'|';
    id += std::to_wstring(rc.right - rc.left);
    id += L'x';
    id += std::to_wstring(rc.bottom - rc.top);
    return id;
}
