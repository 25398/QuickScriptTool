#include "ai_locate_verify.h"

#include "utils.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <functional>
#include <map>

namespace {

/// 最长公共子序列长度（只用于**同一段 CJK 文字**的宽容比较，见 AiElementIndexPartialMatch）。
/// 真正的 LCS：滚动两行 DP，答案是最后一行最后一格 —— 不是沿途最大值。
size_t LcsLength(const std::wstring& a, const std::wstring& b) {
    if (a.empty() || b.empty()) return 0;
    std::vector<size_t> prev(b.size() + 1, 0), cur(b.size() + 1, 0);
    for (size_t i = 1; i <= a.size(); ++i) {
        for (size_t j = 1; j <= b.size(); ++j) {
            cur[j] = (a[i - 1] == b[j - 1])
                ? prev[j - 1] + 1
                : (std::max)(prev[j], cur[j - 1]);
        }
        prev.swap(cur);
        std::fill(cur.begin(), cur.end(), 0);
    }
    return prev[b.size()];
}

/// 是否是「全是 CJK／字母数字」的短标签（没有空格和标点）
bool LooksLikeShortLabel(const std::wstring& s) {
    if (s.size() < 3 || s.size() > 12) return false;
    for (wchar_t c : s) {
        // 中日韩统一表意文字、假名、全角字母数字：算 CJK 标签字符
        const bool cjk = (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3040 && c <= 0x30FF)
            || (c >= 0xFF10 && c <= 0xFF5A);
        const bool ascii = (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'z')
            || (c >= L'A' && c <= L'Z');
        if (!cjk && !ascii) return false;
    }
    return true;
}

struct Box {
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
};

int BoxW(const Box& b) { return (std::max)(0, b.x2 - b.x1); }
int BoxH(const Box& b) { return (std::max)(0, b.y2 - b.y1); }

double IoU(const Box& a, const Box& b) {
    const int ix1 = (std::max)(a.x1, b.x1);
    const int iy1 = (std::max)(a.y1, b.y1);
    const int ix2 = (std::min)(a.x2, b.x2);
    const int iy2 = (std::min)(a.y2, b.y2);
    const int iw = ix2 - ix1;
    const int ih = iy2 - iy1;
    if (iw <= 0 || ih <= 0) return 0.0;
    const double inter = static_cast<double>(iw) * ih;
    const double uni = static_cast<double>(BoxW(a)) * BoxH(a)
        + static_cast<double>(BoxW(b)) * BoxH(b) - inter;
    if (uni <= 0.0) return 0.0;
    return inter / uni;
}

bool CenterInside(const Box& inner, const Box& outer) {
    const int cx = (inner.x1 + inner.x2) / 2;
    const int cy = (inner.y1 + inner.y2) / 2;
    return cx >= outer.x1 && cx <= outer.x2 && cy >= outer.y1 && cy <= outer.y2;
}

std::wstring ToLower(std::wstring s) {
    for (auto& c : s)
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    return s;
}

/// 去掉 UIA 名里的访问键标记（「保存(&S)」→「保存」）与 UI 类型后缀，便于比对
std::wstring NormalizeUiName(const std::wstring& raw) {
    std::wstring s = ToLower(Trim(raw));
    // 去掉 "(&S)" / "(s)" 这类访问键
    if (const size_t p = s.find(L'('); p != std::wstring::npos && s.size() - p <= 5
        && s.find(L')') != std::wstring::npos) {
        s = Trim(s.substr(0, p));
    }
    static const wchar_t* kSuffix[] = {
        L"按钮", L"按键", L"链接", L"输入框", L"菜单项", L"选项", L"图标", L"控件",
    };
    for (auto* suf : kSuffix) {
        const size_t n = wcslen(suf);
        if (s.size() > n && s.compare(s.size() - n, n, suf) == 0) {
            s = Trim(s.substr(0, s.size() - n));
            break;
        }
    }
    return s;
}

size_t LongestCommonSubstr(const std::wstring& a, const std::wstring& b) {
    if (a.empty() || b.empty()) return 0;
    size_t best = 0;
    std::vector<size_t> prev(b.size() + 1, 0);
    std::vector<size_t> cur(b.size() + 1, 0);
    for (size_t i = 1; i <= a.size(); ++i) {
        for (size_t j = 1; j <= b.size(); ++j) {
            cur[j] = (a[i - 1] == b[j - 1]) ? prev[j - 1] + 1 : 0;
            if (cur[j] > best) best = cur[j];
        }
        prev.swap(cur);
        std::fill(cur.begin(), cur.end(), 0);
    }
    return best;
}

}  // namespace

/// 宽容档匹配（CJK 短标签丢字/并字）：OCR 对中文小字常**少读一个字**
/// （「自选僵尸卡牌」读成「自选僵尸卡」，差 1/6 ≈ 17%），而 `AiOcrLabelMatchTier`
/// 的长度比闸（≤40%）在**更短的目标**上会把它挡掉 —— 实测这条让模型在索引里
/// 查不到自己刚看到的文字，于是反复识图 + 反复推敲，一轮烧掉 40 多秒。
///
/// 判据（只在两边都是「短 CJK 标签」时启用，避免误伤长句子与带标点的短语）：
///   · 一方是另一方的子串（顺序完全保留）→ 命中；
///   · 否则 LCS ≥ 较短者的 67% 且**首字相同** → 命中（容忍中间丢/错 1~2 个字）。
/// 返回 1 = 宽容命中。档位**低于**精确/包含档：`AiElementIndexResolve` 里有精确
/// 命中时直接丢弃宽容候选，所以不会出现「模糊的抢走精确的」。
int AiElementIndexPartialMatch(const std::wstring& candidateName,
    const std::wstring& targetDesc) {
    // 局部规范化：去掉空白与常见分隔符（不依赖本文件后面才定义的 OcrLabelNorm，
    // 那个在匿名 namespace 里，外部链接拿不到）
    auto norm = [](const std::wstring& raw) {
        std::wstring out;
        for (wchar_t c : raw) {
            if (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == L'\u3000'
                || c == L':' || c == L'：' || c == L'(' || c == L')' || c == L'（'
                || c == L'）' || c == L'。' || c == L',' || c == L'，' || c == L'、'
                || c == L'-' || c == L'_' || c == L'/' || c == L'\\') {
                continue;
            }
            if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
            out.push_back(c);
        }
        return out;
    };
    const std::wstring cand = norm(candidateName);
    const std::wstring want = norm(targetDesc);
    if (!LooksLikeShortLabel(cand) || !LooksLikeShortLabel(want)) return 0;
    const std::wstring& shorter = cand.size() <= want.size() ? cand : want;
    const std::wstring& longer = cand.size() <= want.size() ? want : cand;
    if (shorter.size() >= 3 && longer.find(shorter) != std::wstring::npos) return 1;
    const size_t lcs = LcsLength(cand, want);
    if (lcs * 3 >= shorter.size() * 2 && cand[0] == want[0]) return 1;
    return 0;
}

bool UiNameMatchesTarget(const std::wstring& name, const std::wstring& target) {
    const std::wstring a = NormalizeUiName(name);
    const std::wstring b = NormalizeUiName(target);
    if (a.empty() || b.empty()) return false;
    if (a == b) return true;
    if (a.size() >= 2 && b.find(a) != std::wstring::npos) return true;
    if (b.size() >= 2 && a.find(b) != std::wstring::npos) return true;
    return LongestCommonSubstr(a, b) >= 2;
}

AiLocateFusionResult FuseLocateCandidates(const std::vector<AiVisionCandidate>& cands,
    const std::vector<AiUiAnchor>& anchors, double iouMatch,
    const std::wstring& targetText) {
    AiLocateFusionResult out;
    out.candidateCount = static_cast<int>(cands.size());
    if (cands.empty()) {
        out.note = L"没有候选";
        return out;
    }
    std::vector<Box> boxes;
    boxes.reserve(cands.size());
    for (const auto& c : cands) {
        Box b{c.x1, c.y1, c.x2, c.y2};
        if (BoxW(b) <= 0 || BoxH(b) <= 0) {
            // 退化成一个点：给个极小的框，便于统一处理
            b.x2 = b.x1 + 1;
            b.y2 = b.y1 + 1;
        }
        boxes.push_back(b);
    }

    // ① UIA 融合：候选与锚点重合（IoU 达标 / 名字对上且部分重合 / 中心落在锚点内）→ 用锚点精确矩形。
    //    取舍依据：IoU ≥ 0.5 时**几何本身就是强证据**（此时锚点面积必然和候选同量级），
    //    名字可以不看；几何弱（只是「候选中心落在锚点里」或勉强重合）时才要求名字对得上。
    const bool requireName = !Trim(targetText).empty();
    for (const auto& anchor : anchors) {
        const Box ab{anchor.x1, anchor.y1, anchor.x2, anchor.y2};
        if (BoxW(ab) <= 0 || BoxH(ab) <= 0) continue;
        const bool nameOk = !requireName || UiNameMatchesTarget(anchor.name, targetText);
        for (const auto& b : boxes) {
            const double iou = IoU(b, ab);
            const bool hit = iou >= iouMatch
                || (iou >= 0.3 && nameOk)
                || (CenterInside(b, ab) && nameOk);
            if (hit) {
                out.ok = true;
                out.uiaConfirmed = true;
                out.uiaName = anchor.name;
                out.boxX1 = ab.x1;
                out.boxY1 = ab.y1;
                out.boxX2 = ab.x2;
                out.boxY2 = ab.y2;
                out.cx = (ab.x1 + ab.x2) / 2;
                out.cy = (ab.y1 + ab.y2) / 2;
                out.clusterSize = static_cast<int>(cands.size());
                out.note = L"UIA 融合命中「" + anchor.name + L"」（用控件精确框"
                    + (nameOk ? L"，名字吻合" : L"，名字不一致但几何重合") + L"）";                return out;
            }
        }
    }

    // ② 单链聚类：合并阈值取「较短边的 0.6」与 24px 的较大者
    const size_t n = boxes.size();
    std::vector<int> parent(n);
    for (size_t i = 0; i < n; ++i) parent[i] = static_cast<int>(i);
    std::function<int(int)> find = [&](int x) {
        while (parent[static_cast<size_t>(x)] != x) {
            parent[static_cast<size_t>(x)] = parent[static_cast<size_t>(parent[static_cast<size_t>(x)])];
            x = parent[static_cast<size_t>(x)];
        }
        return x;
    };
    auto unite = [&](int a, int b) {
        const int ra = find(a);
        const int rb = find(b);
        if (ra != rb) parent[static_cast<size_t>(ra)] = rb;
    };
    for (size_t i = 0; i < n; ++i) {
        for (size_t j = i + 1; j < n; ++j) {
            const int cx1 = (boxes[i].x1 + boxes[i].x2) / 2;
            const int cy1 = (boxes[i].y1 + boxes[i].y2) / 2;
            const int cx2 = (boxes[j].x1 + boxes[j].x2) / 2;
            const int cy2 = (boxes[j].y1 + boxes[j].y2) / 2;
            const int shortI = (std::min)(BoxW(boxes[i]), BoxH(boxes[i]));
            const int shortJ = (std::min)(BoxW(boxes[j]), BoxH(boxes[j]));
            const int ref = (std::max)(24, static_cast<int>(
                (std::min)(shortI, shortJ) * 0.6));
            const long long dx = cx1 - cx2;
            const long long dy = cy1 - cy2;
            if (dx * dx + dy * dy <= static_cast<long long>(ref) * ref) unite(
                static_cast<int>(i), static_cast<int>(j));
        }
    }
    std::vector<std::vector<size_t>> groups(n);
    for (size_t i = 0; i < n; ++i) groups[static_cast<size_t>(find(static_cast<int>(i)))].push_back(i);
    size_t best = 0;
    for (size_t g = 0; g < n; ++g) {
        if (groups[g].size() > groups[best].size()) best = g;
    }
    const auto& grp = groups[best];
    // 簇心 = 簇内候选**框中心的平均**（注意：平均的是「已经聚在一起的候选」，
    // 不是把互相矛盾的候选平均掉——后者研究实测比随机还差）
    long long sx = 0;
    long long sy = 0;
    Box unionBox{INT_MAX, INT_MAX, INT_MIN, INT_MIN};
    for (const size_t idx : grp) {
        sx += (boxes[idx].x1 + boxes[idx].x2) / 2;
        sy += (boxes[idx].y1 + boxes[idx].y2) / 2;
        unionBox.x1 = (std::min)(unionBox.x1, boxes[idx].x1);
        unionBox.y1 = (std::min)(unionBox.y1, boxes[idx].y1);
        unionBox.x2 = (std::max)(unionBox.x2, boxes[idx].x2);
        unionBox.y2 = (std::max)(unionBox.y2, boxes[idx].y2);
    }
    out.ok = true;
    out.clusterSize = static_cast<int>(grp.size());
    out.cx = static_cast<int>(sx / static_cast<long long>(grp.size()));
    out.cy = static_cast<int>(sy / static_cast<long long>(grp.size()));
    if (grp.size() >= 2) {
        out.boxX1 = unionBox.x1;
        out.boxY1 = unionBox.y1;
        out.boxX2 = unionBox.x2;
        out.boxY2 = unionBox.y2;
        out.note = L"聚类一致(" + std::to_wstring(grp.size()) + L"/"
            + std::to_wstring(cands.size()) + L")取簇心";
    } else {
        const size_t idx = grp.front();
        out.boxX1 = boxes[idx].x1;
        out.boxY1 = boxes[idx].y1;
        out.boxX2 = boxes[idx].x2;
        out.boxY2 = boxes[idx].y2;
        out.note = cands.size() >= 2
            ? (L"候选分歧(" + std::to_wstring(cands.size()) + L"个互不相邻)")
            : std::wstring(L"单候选");
    }
    return out;
}

AiLocateVerdict JudgeLocateConfidence(const AiLocateVerifyInput& in,
    std::wstring* outWhy) {
    auto say = [&](const wchar_t* why, AiLocateVerdict v) {
        if (outWhy) *outWhy = why;
        return v;
    };
    if (in.uiaConfirmed) return say(L"UIA 控件确认", AiLocateVerdict::Accept);
    if (in.lowFeature) {
        return say(L"框内特征过低（纯色/空白区），建议放大精炼或换描述",
            AiLocateVerdict::Refine);
    }
    if (in.clusterAgreement < 0.5 && in.clusterAgreement > 0.0) {
        return say(L"多个候选互相矛盾（未聚成一簇）", AiLocateVerdict::Refine);
    }
    if (in.pointOnInteractiveControl) {
        return say(L"该点落在可交互控件上", AiLocateVerdict::Accept);
    }
    const int side = (std::max)(in.boxW, in.boxH);
    if (side > 0 && side < 12) {
        return say(L"定位框过小（可能只是文字噪点）", AiLocateVerdict::Suspect);
    }
    if (in.clusterAgreement >= 1.0 && in.boxW > 0 && in.boxH > 0) {
        return say(L"单候选且框形正常", AiLocateVerdict::Suspect);
    }
    return say(L"证据不足", AiLocateVerdict::Suspect);
}

const wchar_t* AiLocateVerdictName(AiLocateVerdict v) {
    switch (v) {
    case AiLocateVerdict::Accept: return L"可用";
    case AiLocateVerdict::Refine: return L"需精炼";
    default: return L"可疑";
    }
}

std::vector<std::wstring> SplitVisionCandidateLines(const std::wstring& text,
    size_t maxLines) {
    std::vector<std::wstring> out;
    const std::wstring t = Trim(text);
    if (t.empty() || maxLines == 0) return out;
    size_t pos = 0;
    while (pos <= t.size() && out.size() < maxLines) {
        size_t nl = t.find(L'\n', pos);
        if (nl == std::wstring::npos) nl = t.size();
        std::wstring line = Trim(t.substr(pos, nl - pos));
        // 去掉列表前缀：- / * / 1. / 1) 等
        while (!line.empty() && (line[0] == L'-' || line[0] == L'*' || line[0] == L' '
            || line[0] == L'\t')) {
            line.erase(line.begin());
        }
        if (line.size() >= 3 && iswdigit(line[0])) {
            size_t k = 0;
            while (k < line.size() && iswdigit(line[k])) ++k;
            // 只有当数字后面**确实跟着列表分隔符**时才当序号剥掉。
            // 早期版本用「数字/./)/、 一路吃到非分隔符」的写法，会把
            // "500,600" 剥成 ",600"、把 "2、800,900" 剥成 ",900" —— 直接吃掉坐标。
            if (k < line.size() && (line[k] == L'.' || line[k] == L')' || line[k] == L'、')) {
                const wchar_t sep = line[k];
                const size_t after = k + 1;
                // "." 必须后接空白/行尾，避免把 "1.5" 这种数值当序号
                const bool sepOk = (sep != L'.') || after >= line.size()
                    || line[after] == L' ' || line[after] == L'\t';
                if (sepOk && after < line.size()) {
                    const std::wstring rest = Trim(line.substr(after));
                    if (!rest.empty()) line = rest;
                }
            }
        }
        if (!line.empty()) out.push_back(line);
        if (nl == t.size()) break;
        pos = nl + 1;
    }
    return out;
}

bool BitmapRegionLooksLowFeature(HBITMAP bmp, double* outStdDev, double minStdDev) {
    if (outStdDev) *outStdDev = -1.0;
    if (!bmp) return true;
    BITMAP bm{};
    if (!GetObject(bmp, sizeof(bm), &bm) || bm.bmWidth <= 0 || bm.bmHeight <= 0) return true;
    const int w = bm.bmWidth;
    const int h = bm.bmHeight;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;   // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    std::vector<uint8_t> buf(static_cast<size_t>(w) * h * 4);
    HDC dc = GetDC(nullptr);
    if (!dc) return true;
    const int got = GetDIBits(dc, bmp, 0, static_cast<UINT>(h), buf.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    if (got == 0) return true;

    double sum = 0.0;
    double sum2 = 0.0;
    const size_t pixels = static_cast<size_t>(w) * h;
    // 采样步长：大图隔点取，够统计特征了
    const int step = (pixels > 65536) ? 2 : 1;
    size_t n = 0;
    for (int y = 0; y < h; y += step) {
        for (int x = 0; x < w; x += step) {
            const size_t idx = (static_cast<size_t>(y) * w + x) * 4;
            const double b = buf[idx];
            const double g = buf[idx + 1];
            const double r = buf[idx + 2];
            const double lum = 0.299 * r + 0.587 * g + 0.114 * b;
            sum += lum;
            sum2 += lum * lum;
            ++n;
        }
    }
    if (n < 8) return true;
    const double mean = sum / static_cast<double>(n);
    const double var = (std::max)(0.0, sum2 / static_cast<double>(n) - mean * mean);
    const double sd = std::sqrt(var);
    if (outStdDev) *outStdDev = sd;
    return sd < minStdDev;
}

bool AiLocateExtractOcrTarget(const std::wstring& targetDesc, std::wstring* textToFind) {
    std::wstring t = Trim(targetDesc);
    if (t.empty()) return false;

    // ① 剥掉「点击/打开」这类动词前缀（模型常把动作写进目标描述）
    static const wchar_t* kVerbPrefix[] = {
        L"点击", L"单击", L"双击", L"点一下", L"点选", L"选中", L"选择", L"打开", L"按下", L"输入",
    };
    for (auto* v : kVerbPrefix) {
        const size_t n = wcslen(v);
        if (t.size() > n && t.compare(0, n, v) == 0) {
            t = Trim(t.substr(n));
            break;
        }
    }
    // ② 剥掉 UI 类型后缀（「保存按钮」→「保存」）
    static const wchar_t* kSuffix[] = {
        L"按钮", L"按键", L"链接", L"输入框", L"菜单项", L"选项", L"标签页", L"选项卡",
        L"复选框", L"单选框", L"下拉框", L"图标", L"按钮上",
    };
    for (auto* s : kSuffix) {
        const size_t n = wcslen(s);
        if (t.size() > n && t.compare(t.size() - n, n, s) == 0) {
            t = Trim(t.substr(0, t.size() - n));
            break;
        }
    }
    if (t.size() < 2 || t.size() > 12) return false;

    // ③ 目标必须像「屏幕上的一串文字」，否则 OCR 对不上号（宁可跳过核对）
    static const wchar_t* kNotText[] = {
        L"左上", L"右上", L"左下", L"右下", L"左面", L"右面", L"左侧", L"右侧",
        L"上方", L"下方", L"上面", L"下面", L"顶部", L"底部", L"中间", L"中央",
        L"第一个", L"第1个", L"第二个", L"第2个", L"最上", L"最下", L"最左", L"最右",
        L"色", L"圆", L"方框", L"三角", L"对勾", L"勾选", L"箭头", L"图片", L"缩略图",
        L"头像", L"图标", L"标识", L"logo", L"LOGO", L"条形", L"滑块", L"滚动条",
        L"输入区", L"编辑区", L"内容区", L"文本域", L"格子", L"单元格", L"行列",
        L"卡片", L"条目", L"项目", L"列表项", L"缩略", L"区域",
    };
    for (auto* bad : kNotText) {
        if (t.find(bad) != std::wstring::npos) return false;
    }
    // 「第 N 个…」是位置描述，不是屏幕文字
    if (t.find(L"第") != std::wstring::npos && t.find(L"个") != std::wstring::npos) return false;
    // ASCII 标点/空白：多半是坐标、路径或长描述，不是纯标签
    for (const wchar_t c : t) {
        if (c == L' ' || c == L'\t' || c == L',' || c == L'.' || c == L':' || c == L';'
            || c == L'(' || c == L')' || c == L'/' || c == L'\\' || c == L'-' || c == L'_'
            || c == L'\'' || c == L'"' || c == L'[' || c == L']') {
            return false;
        }
    }
    // 至少要有一个汉字或字母数字（纯符号没意义）
    bool hasWord = false;
    for (const wchar_t c : t) {
        if ((c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z')
            || c >= 0x4E00) {
            hasWord = true;
            break;
        }
    }
    if (!hasWord) return false;
    // 纯数字不核对（页码/数量到处都有，OCR「找到」也没意义）
    bool allDigit = true;
    for (const wchar_t c : t) {
        if (!iswdigit(c)) { allDigit = false; break; }
    }
    if (allDigit) return false;

    if (textToFind) *textToFind = t;
    return true;
}

AiLocateOcrOutcome JudgeLocateOcrText(bool ocrUsable, bool found,
    int lineX1, int lineY1, int lineX2, int lineY2,
    int clickX, int clickY, int moveThresholdPx) {
    AiLocateOcrOutcome out;
    if (!ocrUsable) {
        // 没装识别引擎 → 静默跳过（不改判定，也不写日志噪音）
        return out;
    }
    out.checked = true;
    if (!found) {
        out.note = L"未在该区域读到目标文字";
        return out;
    }
    out.found = true;
    if (lineX2 <= lineX1 || lineY2 <= lineY1) {
        out.cx = clickX;
        out.cy = clickY;
        out.note = L"文本核对通过（识别行无有效框）";
        return out;
    }
    const int cx = (lineX1 + lineX2) / 2;
    const int cy = (lineY1 + lineY2) / 2;
    out.dx = cx - clickX;
    out.dy = cy - clickY;
    out.cx = cx;
    out.cy = cy;
    if (std::abs(out.dx) <= moveThresholdPx && std::abs(out.dy) <= moveThresholdPx) {
        out.note = L"文本核对通过（位置一致）";
        return out;
    }
    // 偏得多 → 按 OCR 识别框修正（夹在行框内，避免落到文字外侧）
    const int minX = lineX1 + 2;
    const int maxX = (std::max)(minX, lineX2 - 2);
    const int minY = lineY1 + 1;
    const int maxY = (std::max)(minY, lineY2 - 1);
    out.cx = (std::max)(minX, (std::min)(maxX, cx));
    out.cy = (std::max)(minY, (std::min)(maxY, cy));
    out.moved = true;
    out.note = L"文本核对通过，按识别框修正 " + std::to_wstring(out.dx) + L","
        + std::to_wstring(out.dy) + L" 像素";
    return out;
}

// ── 「文字直点」：本地 OCR 索引 → 直接点击 ─────────────────────────────
namespace {

/// OCR 比对用的归一化：去空白/全角空白、去常见全半角标点、ASCII 转小写。
/// 目的只有一个：让「自选僵尸卡牌」和「自选僵尸卡牌 」/「登录」与「登录:」能对上号。
std::wstring OcrLabelNorm(const std::wstring& raw) {
    std::wstring s;
    s.reserve(raw.size());
    for (wchar_t c : raw) {
        if (c == L' ' || c == L'\t' || c == L'\u3000' || c == L'\r' || c == L'\n') continue;
        if (c == L'：' || c == L':' || c == L'，' || c == L',' || c == L'。' || c == L'.'
            || c == L'*' || c == L'·' || c == L'-' || c == L'_' || c == L'|' || c == L'/') {
            continue;
        }
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
        s.push_back(c);
    }
    return s;
}

bool OcrLineBoxSane(const OcrTextLine& ln, int screenW, int screenH) {
    const int w = ln.x2 - ln.x1;
    const int h = ln.y2 - ln.y1;
    if (w < 8 || h < 8) return false;
    // 整块面板被并成「一行」时框会很大：点它的中心会打到别的东西
    if (screenW > 0 && w > (std::max)(80, screenW / 6)) return false;
    if (screenH > 0 && h > (std::max)(28, screenH / 6)) return false;
    if (screenW > 0 && screenH > 0
        && static_cast<double>(w) * h > 0.03 * screenW * screenH) {
        return false;
    }
    return true;
}

/// 匹配档位：2 = 完全相等，1 = 双向包含且长度接近，0 = 不匹配。
/// **不做模糊匹配**：OCR 模糊命中很容易点到隔壁相似按钮上，宁可回落识图。
int OcrLabelMatchTier(const std::wstring& lineNorm, const std::wstring& needle) {
    if (lineNorm.empty() || needle.empty()) return 0;
    if (lineNorm == needle) return 2;
    const size_t lo = (std::min)(lineNorm.size(), needle.size());
    const size_t hi = (std::max)(lineNorm.size(), needle.size());
    if (lo * 5 < hi * 3) return 0;   // 长度差 > 40%：不是同一个标签
    if (lineNorm.find(needle) != std::wstring::npos
        || needle.find(lineNorm) != std::wstring::npos) {
        return 1;
    }
    return 0;
}

void OcrCollectTieredMatches(const std::vector<OcrTextLine>& lines,
    const std::wstring& needle, int screenW, int screenH,
    std::vector<OcrTextLine> tiersOut[2]) {
    for (const auto& ln : lines) {
        if (ln.confidence > 0.0 && ln.confidence < 0.5) continue;
        const int tier = OcrLabelMatchTier(OcrLabelNorm(ln.text), needle);
        if (tier <= 0) continue;
        if (!OcrLineBoxSane(ln, screenW, screenH)) continue;
        // ★索引 0 必须是「完全相等」档：tier 2=相等、1=包含，所以是 2-tier
        //   （写成 tier-1 会把两档颠倒 —— 自检 ocr_direct_click_pick 当场抓到过）
        tiersOut[2 - tier].push_back(ln);
    }
}

/// 同一文字行：垂直方向重叠够多（按钮/标签的文字被 OCR 拆成多段时用）。
bool OcrLinesSameRow(const OcrTextLine& a, const OcrTextLine& b) {
    const int top = (std::max)(a.y1, b.y1);
    const int bot = (std::min)(a.y2, b.y2);
    const int overlap = bot - top;
    if (overlap <= 0) return false;
    const int ha = (std::max)(1, a.y2 - a.y1);
    const int hb = (std::max)(1, b.y2 - b.y1);
    return overlap * 2 >= (std::min)(ha, hb);   // 重叠 ≥ 较矮者的 50%
}

/// 把同一行上**横向相邻**的多段文字拼成候选（最多 maxRun 段）。
///
/// 为什么要这一步（实测日志）：OCR 常把一个按钮的文字拆成多段 ——
/// 左上角「自选僵尸卡牌」整条没进索引、右上角「一键全选」只读到「键全选」。
/// 单段既过不了长度比（「僵尸卡牌」4 字 vs 目标 6 字，差 >40% → 档位 0），
/// 也过不了框尺寸合理性检查（整块并成一行时框太大直接被丢）。
/// 拼接之后文本与框都对得上，文字直点才有机会命中。
std::vector<OcrTextLine> OcrMergedRowCandidates(const std::vector<OcrTextLine>& lines,
    size_t maxRun = 4) {
    std::vector<OcrTextLine> sorted;
    for (const auto& ln : lines) {
        if (ln.confidence > 0.0 && ln.confidence < 0.5) continue;
        if (ln.text.empty()) continue;
        sorted.push_back(ln);
    }
    // 阅读顺序：先上行、再从左到右（拼接必须是相邻段，所以要稳定排序）
    std::sort(sorted.begin(), sorted.end(), [](const OcrTextLine& a, const OcrTextLine& b) {
        if (a.y1 != b.y1) return a.y1 < b.y1;
        return a.x1 < b.x1;
    });

    std::vector<OcrTextLine> out;
    for (size_t i = 0; i < sorted.size(); ++i) {
        OcrTextLine acc = sorted[i];
        for (size_t n = 1; n < maxRun && i + n < sorted.size(); ++n) {
            const OcrTextLine& nxt = sorted[i + n];
            if (!OcrLinesSameRow(acc, nxt)) break;
            // 只拼横向紧邻的：间隙不能超过「已拼高度的 1.5 倍」（避免把整行按钮全并起来）
            const int gap = nxt.x1 - acc.x2;
            const int h = (std::max)(1, acc.y2 - acc.y1);
            if (gap > h * 3 / 2 || gap < -h) break;
            OcrTextLine merged;
            merged.text = acc.text + nxt.text;
            merged.x1 = (std::min)(acc.x1, nxt.x1);
            merged.y1 = (std::min)(acc.y1, nxt.y1);
            merged.x2 = (std::max)(acc.x2, nxt.x2);
            merged.y2 = (std::max)(acc.y2, nxt.y2);
            merged.confidence = (std::min)(acc.confidence, nxt.confidence);
            acc = merged;
            out.push_back(merged);   // 2 段、3 段… 每个长度都当候选
        }
    }
    return out;
}

}  // namespace

bool AiOcrPickDirectClickTarget(const std::vector<OcrTextLine>& lines,
    const std::wstring& wantText, int screenW, int screenH,
    AiOcrDirectHit* out) {
    AiOcrDirectHit hit;
    const std::wstring needle = OcrLabelNorm(wantText);
    if (needle.size() < 2) {
        hit.why = L"目标太短，不做文字直点";
        if (out) *out = hit;
        return false;
    }

    // 两档匹配：完全相等 > 双向包含（长度接近）。
    std::vector<OcrTextLine> tiers[2];
    OcrCollectTieredMatches(lines, needle, screenW, screenH, tiers);

    // ★同一行多段拼接后再匹配一轮（见 OcrMergedRowCandidates 注释）。
    //   合并候选是单段的**超集**（「自选」+「僵尸卡牌」→「自选僵尸卡牌」），
    //   所以不能按位置去重（中心天然不同），改为在**歧义判定**里放过
    //   「一个框基本套住另一个」的情况 —— 那是同一处的两种读法，不是两个按钮。
    for (const auto& merged : OcrMergedRowCandidates(lines)) {
        const int tier = OcrLabelMatchTier(OcrLabelNorm(merged.text), needle);
        if (tier <= 0) continue;
        if (!OcrLineBoxSane(merged, screenW, screenH)) continue;
        tiers[2 - tier].push_back(merged);
    }

    bool havePick = false;
    OcrTextLine best{};
    const wchar_t* pickKind = L"";
    std::wstring fallbackWhy = L"OCR 索引里没有「" + wantText + L"」";

    /// 两个框是否「基本是同一处」：交集 ≥ 较小框面积的 80%。
    /// 用途：同一段文字可能有多种读法（单段「僵尸卡牌」与拼接后的「自选僵尸卡牌」），
    /// 它们是同一处，**不算歧义**；把这层判断放在歧义检测里，才不会
    /// 因为多了一种读法就放弃直点（那是把优化当噪声丢掉）。
    auto sameSpot = [](const OcrTextLine& a, const OcrTextLine& b) {
        const int ix = (std::min)(a.x2, b.x2) - (std::max)(a.x1, b.x1);
        const int iy = (std::min)(a.y2, b.y2) - (std::max)(a.y1, b.y1);
        if (ix <= 0 || iy <= 0) return false;
        const long long inter = static_cast<long long>(ix) * iy;
        const long long areaA = static_cast<long long>((std::max)(1, a.x2 - a.x1))
            * (std::max)(1, a.y2 - a.y1);
        const long long areaB = static_cast<long long>((std::max)(1, b.x2 - b.x1))
            * (std::max)(1, b.y2 - b.y1);
        const long long smaller = (std::min)(areaA, areaB);
        return inter * 10 >= smaller * 8;
    };

    for (int t = 0; t < 2 && !havePick; ++t) {
        if (tiers[t].empty()) continue;
        const std::vector<OcrTextLine>& sane = tiers[t];
        // 唯一性：同一档里出现多个相距较远的候选 = 同屏多个同名按钮 → 歧义
        const int cx0 = (sane[0].x1 + sane[0].x2) / 2;
        const int cy0 = (sane[0].y1 + sane[0].y2) / 2;
        bool ambiguous = false;
        size_t farCount = 0;
        for (size_t i = 1; i < sane.size(); ++i) {
            const int cx = (sane[i].x1 + sane[i].x2) / 2;
            const int cy = (sane[i].y1 + sane[i].y2) / 2;
            if (std::abs(cx - cx0) <= 16 && std::abs(cy - cy0) <= 16) continue;
            if (sameSpot(sane[0], sane[i])) continue;   // 同一处的另一种读法
            ++farCount;
            ambiguous = true;
            break;
        }
        if (ambiguous) {
            fallbackWhy = L"OCR 同屏有多个「" + wantText + L"」（"
                + std::to_wstring(farCount + 1) + L" 处），歧义不做直点";
            continue;
        }
        // 多个同处候选时取**最长文本**那条：拼接后的完整读法框更贴合整颗按钮，
        // 点它的中心比点残段更准。
        best = sane[0];
        for (const auto& c : sane) {
            if (c.text.size() > best.text.size()) best = c;
        }
        pickKind = (t == 0) ? L"exact" : L"contains";
        havePick = true;
    }

    if (!havePick) {
        hit.why = fallbackWhy;
        if (out) *out = hit;
        return false;
    }
    hit.screenX = (best.x1 + best.x2) / 2;
    hit.screenY = (best.y1 + best.y2) / 2;
    hit.boxW = best.x2 - best.x1;
    hit.boxH = best.y2 - best.y1;
    hit.hitText = best.text;
    hit.matchKind = pickKind;
    if (out) *out = hit;
    return true;
}

// ── 屏幕文字索引（「可点清单」）──────────────────────────────────────────
namespace {

/// 纯数字条目的**额度**（不是「只留 6 条」——为什么放宽见下面两遍收集的注释）。
/// ⚠ 6 是历史值，实测**规模一大就断**：一张 14 张卡的选卡界面有 14 个价签，
///   能进索引的只有 6 个，而且按 OCR 顺序截断 ⇒「哪些价格留下」纯属偶然
///   ⇒ 模型按价格找卡必然「索引里没有 600/800/9999」，只好拿一个裸数字去问 VLM
///   （屏上多个数字 → 每次给的框都不一样）⇒ 点空 ⇒ 死点表/近点重复连环触发（docs §44）。
constexpr int kOcrIndexMaxNumericSpans = 16;

/// 一段文字是否值得进索引：长度合理 + 置信度够。
/// 纯数字（价格/血量/分数）不是可点按钮，**限量**保留 —— 实测满屏价格会把索引
/// 塞满（33 条全是 100/75/6666…），模型反而找不到「一键全选/上一页/确认」这类文字按钮。
/// ⚠ 但「限量」**不能按 OCR 顺序截断**（那等于抽签）：调用方分两遍收集，
///   带文字的条目先占位，纯数字只在还有额度时补进来 —— 两边的意图同时保住。
/// `maxNumericSpans < 0` = 用默认闸；传大值 = **不限量**（版面推断要用全量，见头文件）。
bool OcrSpanWorthIndexing(const std::wstring& t, double conf, int numericKept,
    int maxNumericSpans = -1) {
    if (t.size() < 2 || t.size() > 24) return false;
    if (conf < 0.55) return false;
    bool hasLetter = false;
    for (const wchar_t c : t) {
        if ((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || c >= 0x4E00) {
            hasLetter = true;
            break;
        }
    }
    if (!hasLetter) {
        const int cap = maxNumericSpans >= 0 ? maxNumericSpans : kOcrIndexMaxNumericSpans;
        return numericKept < cap;
    }
    return true;
}

bool OcrSpanHasLetter(const std::wstring& t) {
    for (const wchar_t c : t) {
        if ((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || c >= 0x4E00) return true;
    }
    return false;
}

}  // namespace

std::vector<std::vector<OcrIndexSpan>> CollectOcrIndexRows(
    const std::vector<OcrTextLine>& lines, size_t maxSpans, int maxNumericSpans,
    int maxGapFactor) {
    std::vector<OcrIndexSpan> spans;
    auto makeSpan = [](const OcrTextLine& ln, const std::wstring& t) {
        OcrIndexSpan sp;
        sp.text = t;
        sp.x1 = ln.x1; sp.y1 = ln.y1; sp.x2 = ln.x2; sp.y2 = ln.y2;
        return sp;
    };
    // ★★两遍收集（docs §44）：**带文字的条目先占位**，纯数字在还有额度时再补。
    //   单遍 + 按 OCR 顺序截断纯数字的本意是对的（满屏价格会把「一键全选/上一页/确认」
    //   这类文字按钮挤没），但它让"哪些价格留下"变成抽签，规模一大（14 张卡=14 个价签）
    //   就必然缺；而模型手上的选卡清单**恰恰是按价格写的**（见 planSpend 的 costs 简写）。
    //   两遍收集两边都保住：文字条目永不因数字缺席；有余量时价签/编号都在索引里
    //   ⇒「按价格/编号选」真的能用（这条承诺见 ai_locate_verify.h 的档位说明）。
    int numericKept = 0;
    for (const auto& ln : lines) {
        const std::wstring t = Trim(ln.text);
        if (!OcrSpanWorthIndexing(t, ln.confidence, 0)) continue;
        if (!OcrSpanHasLetter(t)) continue;   // 数字留到第二遍
        spans.push_back(makeSpan(ln, t));
    }
    for (const auto& ln : lines) {
        const std::wstring t = Trim(ln.text);
        if (OcrSpanHasLetter(t)) continue;    // 第一遍已收
        if (!OcrSpanWorthIndexing(t, ln.confidence, numericKept, maxNumericSpans)) continue;
        ++numericKept;
        spans.push_back(makeSpan(ln, t));
    }
    if (spans.empty()) return {};
    // 阅读顺序：先上后下、再从左到右（分组依赖相邻性，必须稳定排序）
    std::sort(spans.begin(), spans.end(), [](const OcrIndexSpan& a, const OcrIndexSpan& b) {
        if (a.y1 != b.y1) return a.y1 < b.y1;
        return a.x1 < b.x1;
    });

    std::vector<std::vector<OcrIndexSpan>> rows;
    for (const auto& sp : spans) {
        bool placed = false;
        if (!rows.empty()) {
            const auto& cur = rows.back();
            const OcrIndexSpan& last = cur.back();
            const int top = (std::max)(last.y1, sp.y1);
            const int bot = (std::min)(last.y2, sp.y2);
            const int hLast = (std::max)(1, last.y2 - last.y1);
            const int hCur = (std::max)(1, sp.y2 - sp.y1);
            const int hRef = (std::max)(hLast, hCur);
            const bool sameRow = (bot - top) * 2 >= (std::min)(hLast, hCur);
            const int gap = sp.x1 - last.x2;
            // ⚠ 别把变量叫 `near`：Windows SDK 里它是宏/关键字，MSVC 会报 C2513。
            const bool closeEnough = gap <= hRef * maxGapFactor && gap >= -hRef;
            if (sameRow && closeEnough && cur.size() < maxSpans) {
                rows.back().push_back(sp);
                placed = true;
            }
        }
        if (!placed) rows.push_back({ sp });
    }
    return rows;
}

std::wstring FormatOcrTextIndex(const std::vector<OcrTextLine>& lines,
    int capX1, int capY1, int capX2, int capY2,
    int frameW, int frameH, int* outItemCount) {
    const std::vector<std::vector<OcrIndexSpan>> rows = CollectOcrIndexRows(lines);
    if (outItemCount) *outItemCount = 0;
    if (rows.empty()) return {};

    const int capH = (std::max)(1, capY2 - capY1);
    const int capW = (std::max)(1, capX2 - capX1);
    // ★★坐标必须换成 **upload 截图像素** —— `mouseClick(x,y)` 收的就是这一套。
    //   旧版直接发 OCR 的**屏幕绝对像素**（`centerX()` 就是屏幕坐标），标题却写着
    //   「与 mouseClick 同一套」：在 2560×1440 截成 1024×576 的帧里差 2.5 倍，
    //   模型照它点必然落空。§33.1 只修了 `FormatAiElementIndex` 那一处，
    //   **同一句话在文字索引里原样留着**（docs §60.5）。
    const bool haveFrame = frameW > 1 && frameH > 1;
    auto toFrameX = [&](int sx) {
        return haveFrame ? std::clamp(static_cast<int>(
            static_cast<double>(sx - capX1) * frameW / capW), 0, frameW - 1) : sx;
    };
    auto toFrameY = [&](int sy) {
        return haveFrame ? std::clamp(static_cast<int>(
            static_cast<double>(sy - capY1) * frameH / capH), 0, frameH - 1) : sy;
    };
    std::wstring idx;
    int total = 0;
    for (size_t ri = 0; ri < rows.size(); ++ri) {
        const auto& row = rows[ri];
        if (row.empty()) continue;
        // 视觉行序 + 该行在画面里的纵向位置（%）：光给绝对像素，模型推不出「靠上还是靠下」
        const int rowCy = (row.front().y1 + row.front().y2) / 2;
        const int pct = std::clamp((rowCy - capY1) * 100 / capH, 0, 100);
        idx += L"第" + std::to_wstring(ri + 1) + L"行[" + std::to_wstring(pct) + L"%] ";
        for (size_t si = 0; si < row.size(); ++si) {
            if (si) idx += L" · ";
            idx += row[si].text + L"(" + std::to_wstring(toFrameX(row[si].centerX()))
                + L"," + std::to_wstring(toFrameY(row[si].centerY())) + L")";
            ++total;
        }
        idx += L"\n";
    }
    if (outItemCount) *outItemCount = total;
    // 口径说明按**实际给出的那一套**写（判据的宾语是事情本身：给什么就说什么）
    const std::wstring coordNote = haveFrame
        ? (L"坐标为 **upload 截图像素**（和 mouseClick(x,y) 完全同一套），图像尺寸 "
            + std::to_wstring(frameW) + L"×" + std::to_wstring(frameH) + L"；")
        : std::wstring(L"⚠ 本次**没有帧尺寸**可换算，下面的坐标是**屏幕绝对像素**，"
            L"**不能**直接填进 mouseClick（它会按 upload 像素解释）；");
    return L"屏幕文字索引（本地 OCR；" + coordNote
        + L"已按画面上的**行**分组，同一行内从左到右 = 卡片/按钮的先后顺序）：\n" + idx;
}

std::wstring AiDescribeTextIndexDelta(const std::vector<OcrTextLine>& prev,
    const std::vector<OcrTextLine>& cur, size_t maxItems) {
    if (prev.empty() || cur.empty() || maxItems == 0) return std::wstring();
    const auto normOf = [](const OcrTextLine& l) { return OcrLabelNorm(l.text); };
    // 多重集差：把上一帧的文本记数，再减去这一帧的 ⇒ 剩下的就是"消失了多少"
    std::map<std::wstring, int> prevCount;
    for (const auto& l : prev) {
        const std::wstring n = normOf(l);
        if (!n.empty()) ++prevCount[n];
    }
    std::vector<OcrTextLine> appeared;
    for (const auto& l : cur) {
        const std::wstring n = normOf(l);
        if (n.empty()) continue;
        auto it = prevCount.find(n);
        if (it != prevCount.end() && it->second > 0) { --it->second; continue; }
        appeared.push_back(l);
    }
    std::vector<OcrTextLine> disappeared;
    for (const auto& kv : prevCount) {
        for (int k = 0; k < kv.second; ++k) {
            OcrTextLine l{};
            l.text = kv.first;
            // 位置用于数字配对：从上一帧里找一条同名文本照抄它的框
            for (const auto& p : prev) {
                if (normOf(p) == kv.first) {
                    l.x1 = p.x1; l.y1 = p.y1; l.x2 = p.x2; l.y2 = p.y2;
                    break;
                }
            }
            disappeared.push_back(l);
        }
    }
    if (appeared.empty() && disappeared.empty()) return std::wstring();

    // 纯数字按**最近的 x 位置**配成「数值 A → B」（计数器变化是最要紧也最常见的一种；
    // 数字自带对齐语义，所以只有它配得起，整行文字不配 —— OCR 的框每帧都在抖）。
    const auto isNumeric = [](const std::wstring& t) {
        if (t.empty() || t.size() > 8) return false;
        for (wchar_t c : t) if (c < L'0' || c > L'9') return false;
        return true;
    };
    std::wstring numericText;
    std::vector<char> usedA(appeared.size(), 0), usedD(disappeared.size(), 0);
    for (size_t di = 0; di < disappeared.size(); ++di) {
        if (!isNumeric(disappeared[di].text)) continue;
        int best = -1, bestDx = 260;   // 260px ≈ 计数器数字不会跑更远（帧抖 + 位数变化）
        const int dCx = (disappeared[di].x1 + disappeared[di].x2) / 2;
        for (size_t ai = 0; ai < appeared.size(); ++ai) {
            if (usedA[ai] || !isNumeric(appeared[ai].text)) continue;
            if (appeared[ai].text == disappeared[di].text) continue;
            const int aCx = (appeared[ai].x1 + appeared[ai].x2) / 2;
            const int dx = std::abs(aCx - dCx);
            if (dx < bestDx) { bestDx = dx; best = static_cast<int>(ai); }
        }
        if (best < 0) continue;
        usedD[di] = 1;
        usedA[static_cast<size_t>(best)] = 1;
        if (!numericText.empty()) numericText += L"；";
        numericText += L"\"" + disappeared[di].text + L"\" → \"" + appeared[static_cast<size_t>(best)].text + L"\"";
    }

    std::wstring added, removed;
    size_t shown = 0;
    for (size_t ai = 0; ai < appeared.size() && shown < maxItems; ++ai) {
        if (usedA[ai]) continue;
        if (!added.empty()) added += L"、";
        added += L"\"" + appeared[ai].text + L"\"";
        ++shown;
    }
    for (size_t di = 0; di < disappeared.size() && shown < maxItems; ++di) {
        if (usedD[di]) continue;
        if (!removed.empty()) removed += L"、";
        removed += L"\"" + disappeared[di].text + L"\"";
        ++shown;
    }
    std::wstring out = L"[事实] 屏幕文字变化（本地 OCR 帧间差分，零额外识图）：";
    if (!numericText.empty()) out += L"数值 " + numericText + L"；";
    if (!added.empty()) out += L"新出现 " + added + L"；";
    if (!removed.empty()) out += L"不再出现 " + removed + L"；";
    if (out.back() == L'；') out.pop_back();
    out += L"。";
    return out;
}

// 见头文件注释：要求复核「完全相等」会让复核永远比索引更严 → 永远回落识图。
// ── 索引档 vs 就地复核档：文字直点的采信规则 ────────────────────────────
// 见头文件注释：要求复核「完全相等」会让复核永远比索引更严 → 永远回落识图。
int AiOcrLabelMatchTier(const std::wstring& lineText, const std::wstring& wantText) {
    return OcrLabelMatchTier(OcrLabelNorm(lineText), OcrLabelNorm(wantText));
}

bool AiOcrProbeAgreesWithIndex(const std::wstring& probeText,
    const std::wstring& indexHitText, const std::wstring& wantText,
    int probeX, int probeY, int indexX, int indexY,
    int minIndexTier, int maxDriftPx) {
    // ① 复核读到的必须仍算「同一个标签」
    if (AiOcrLabelMatchTier(probeText, wantText) <= 0) return false;
    // ② 索引那次命中本身要够好（默认 contains 档就行）
    if (AiOcrLabelMatchTier(indexHitText, wantText) < minIndexTier) return false;
    // ③ 复核位置不能跑到别处去（复核的价值是把坐标修得更准，不是换个目标）
    const long long dx = probeX - indexX;
    const long long dy = probeY - indexY;
    const long long drift2 = dx * dx + dy * dy;
    if (maxDriftPx > 0 && drift2 > static_cast<long long>(maxDriftPx) * maxDriftPx)
        return false;
    return true;
}

bool AiOcrPickNearestText(const std::vector<OcrTextLine>& lines,
    const std::wstring& wantText, int nearX, int nearY, int maxDistPx,
    int screenW, int screenH, AiOcrDirectHit* out) {
    AiOcrDirectHit hit;
    const std::wstring needle = OcrLabelNorm(wantText);
    if (needle.size() < 2) {
        hit.why = L"目标太短，不做文字核位";
        if (out) *out = hit;
        return false;
    }
    std::vector<OcrTextLine> tiers[2];
    OcrCollectTieredMatches(lines, needle, screenW, screenH, tiers);

    std::wstring why = L"OCR 里没有「" + wantText + L"」";
    for (int t = 0; t < 2; ++t) {
        if (tiers[t].empty()) continue;
        // 「最近的同名文字」比「全局唯一」实用：识图点通常已经离目标不远，
        // 同屏其它同名按钮（页脚「确定」）会被距离排掉。
        const OcrTextLine* best = nullptr;
        long long bestD2 = 0;
        for (const auto& ln : tiers[t]) {
            const long long cx = (ln.x1 + ln.x2) / 2;
            const long long cy = (ln.y1 + ln.y2) / 2;
            const long long dx = cx - nearX;
            const long long dy = cy - nearY;
            const long long d2 = dx * dx + dy * dy;
            if (maxDistPx > 0 && d2 > static_cast<long long>(maxDistPx) * maxDistPx) continue;
            if (!best || d2 < bestD2) {
                best = &ln;
                bestD2 = d2;
            }
        }
        if (!best) {
            why = L"OCR 有「" + wantText + L"」但都在 " + std::to_wstring(maxDistPx)
                + L"px 之外（疑似另一处同名文字）";
            continue;
        }
        hit.screenX = (best->x1 + best->x2) / 2;
        hit.screenY = (best->y1 + best->y2) / 2;
        hit.boxW = best->x2 - best->x1;
        hit.boxH = best->y2 - best->y1;
        hit.hitText = best->text;
        hit.matchKind = (t == 0) ? L"exact" : L"contains";
        if (out) *out = hit;
        return true;
    }
    hit.why = why;
    if (out) *out = hit;
    return false;
}

// ── 统一元素索引：把「看得见的可点东西」编成一张表（所见即所得）──────────
// 设计动机与规则见头文件；这里只强调两条硬约束：
//   ① **窗口自身标题栏按钮永不参与按名字解析**（实测靠名字挑「关闭」关掉了整个游戏窗口）；
//   ② 合并要保守 —— 宁可同一处出现两条，也不要把两个控件并成一条（并错 = 点错东西）。
const wchar_t* AiElementSourceName(AiElementSource s) {
    switch (s) {
    case AiElementSource::UiAutomation: return L"控件";
    case AiElementSource::OcrText: return L"文字";
    case AiElementSource::LabeledIcon: return L"图标槽";
    case AiElementSource::Vision: return L"识图";
    }
    return L"?";
}

namespace {

int IndexClampRectW(const AiElementEntry& e) { return e.x2 - e.x1; }
int IndexClampRectH(const AiElementEntry& e) { return e.y2 - e.y1; }

double IndexRectIou(int ax1, int ay1, int ax2, int ay2,
    int bx1, int by1, int bx2, int by2) {
    const int ix1 = (std::max)(ax1, bx1);
    const int iy1 = (std::max)(ay1, by1);
    const int ix2 = (std::min)(ax2, bx2);
    const int iy2 = (std::min)(ay2, by2);
    if (ix2 <= ix1 || iy2 <= iy1) return 0.0;
    const double inter = static_cast<double>(ix2 - ix1) * (iy2 - iy1);
    const double areaA = static_cast<double>(ax2 - ax1) * (ay2 - ay1);
    const double areaB = static_cast<double>(bx2 - bx1) * (by2 - by1);
    const double uni = areaA + areaB - inter;
    return uni > 0.0 ? inter / uni : 0.0;
}

bool IndexPointInside(int x, int y, int x1, int y1, int x2, int y2) {
    return x >= x1 && x < x2 && y >= y1 && y < y2;
}

/// 「同一处」：IoU 够高，或一方中心落在另一方里（小按钮套在容器框里时 IoU 会偏低）
bool IndexSameSpot(int ax1, int ay1, int ax2, int ay2, int bx1, int by1, int bx2, int by2,
    double iouThreshold) {
    if (IndexRectIou(ax1, ay1, ax2, ay2, bx1, by1, bx2, by2) >= iouThreshold) return true;
    const int acx = (ax1 + ax2) / 2;
    const int acy = (ay1 + ay2) / 2;
    if (IndexPointInside(acx, acy, bx1, by1, bx2, by2)) return true;
    const int bcx = (bx1 + bx2) / 2;
    const int bcy = (by1 + by2) / 2;
    return IndexPointInside(bcx, bcy, ax1, ay1, ax2, ay2);
}

/// 目标净化：去掉 UI 类型词/标点，用于档位比较（复用 OCR 侧的规范化）
std::wstring IndexNormTarget(const std::wstring& raw) { return OcrLabelNorm(raw); }

/// 纯数字目标（「50」「600」）不参与解析 —— 满屏都是，猜必错。
/// 只有 1 个字的目标同理（「卡」「点」）。
bool IndexTargetTooAmbiguous(const std::wstring& normTarget) {
    if (normTarget.size() < 2) return true;
    bool allDigit = true;
    for (wchar_t c : normTarget) {
        if (c < L'0' || c > L'9') {
            allDigit = false;
            break;
        }
    }
    return allDigit;
}

}  // namespace

std::vector<AiElementEntry> BuildAiElementIndexWithIcons(
    const std::vector<AiUiAnchor>& uiControls,
    const std::vector<AiIndexRowInput>& ocrRows,
    const std::vector<AiLabelIconPair>& iconSlots,
    size_t maxItems, double dedupIou) {
    std::vector<AiElementEntry> out;
    if (maxItems == 0) maxItems = 80;
    if (dedupIou <= 0.0) dedupIou = 0.35;

    // ① UIA 控件先入表（矩形是控件真框、还带 InvokePattern 能力位 → 优先保留）
    for (const auto& ui : uiControls) {
        if (ui.name.empty()) continue;
        if (ui.x2 <= ui.x1 || ui.y2 <= ui.y1) continue;
        AiElementEntry e;
        e.source = AiElementSource::UiAutomation;
        e.name = ui.name;
        e.x1 = ui.x1;
        e.y1 = ui.y1;
        e.x2 = ui.x2;
        e.y2 = ui.y2;
        // ★★如实透传角色 / 动作能力 / 可读状态（见 AiUiAnchor 的理由）：
        //   这三样正是「半视觉」要补的东西 —— 模型据此不必靠截图猜
        //   「这是按钮还是输入框」，也不必先点一下才知道开关当前是开还是关。
        //   ⚠ 只搬事实，不在这里做任何加工/判断（加工会引入本层不该有的策略）。
        e.role = ui.role;
        e.action = ui.action;
        e.state = ui.state;
        // AiUiAnchor 没有 chrome/enabled 字段（那是 window_mode 侧的 UiControlInfo 才有的）；
        // 调用方在填充 aiUiAnchors 前已按 chrome 过滤，这里只做形状校验。
        out.push_back(std::move(e));
    }

    // ② ★「短标签 → 它标注的图标槽」（引擎按版面推断，见 PairCaptionRowWithIconBand）。
    //    这条是**推断**出来的坐标（不是读出来的）⇒ 证据一起发给模型（`note`），
    //    并且**取代**原来那条价签文字条目（否则同名两条 ⇒ 解析判歧义 ⇒ 等于白做）。
    for (const auto& p : iconSlots) {
        if (p.x2 <= p.x1 || p.y2 <= p.y1 || p.label.text.empty()) continue;
        AiElementEntry e;
        e.source = AiElementSource::LabeledIcon;
        e.name = p.label.text;
        e.x1 = p.x1;
        e.y1 = p.y1;
        e.x2 = p.x2;
        e.y2 = p.y2;
        e.note = L"引擎按版面推断：本行 " + std::to_wstring(p.labelCount)
            + L" 段短标签等距，它们**正上方**有 " + std::to_wstring(p.slotCount)
            + L" 个高约 " + std::to_wstring(p.iconHeight) + L"px 的等距块 ⇒ 本条是那个块（图标）"
              L"的位置，不是文字；标签文字本身在 ("
            + std::to_wstring((p.label.x1 + p.label.x2) / 2) + L","
            + std::to_wstring((p.label.y1 + p.label.y2) / 2) + L")";
        out.push_back(std::move(e));
    }

    // ③ OCR 行合并进来：同一处 + 名字对得上才并入（保留 UIA 矩形），否则另起一条
    for (const auto& row : ocrRows) {
        if (row.text.empty()) continue;
        if (row.x2 <= row.x1 || row.y2 <= row.y1) continue;
        // 已被「标签→图标槽」配对消费掉的标签**不再单独进表**：同一句话两条同档，
        // `locateAndClick` 会判歧义拒绝（那是我们自己的两条记录在打架，不是模型的问题）。
        bool consumed = false;
        for (const auto& p : iconSlots) {
            if (p.label.text == row.text && p.label.x1 == row.x1 && p.label.y1 == row.y1) {
                consumed = true;
                break;
            }
        }
        if (consumed) continue;
        bool merged = false;
        for (auto& e : out) {
            if (!IndexSameSpot(e.x1, e.y1, e.x2, e.y2, row.x1, row.y1, row.x2, row.y2, dedupIou))
                continue;
            if (!UiNameMatchesTarget(e.name, row.text) && !UiNameMatchesTarget(row.text, e.name))
                continue;
            // 同一处、同一意思：并成一条。名字保留 UIA 的（更规范），坐标保留 UIA 的（控件真框）。
            merged = true;
            break;
        }
        if (merged) continue;
        AiElementEntry e;
        e.source = AiElementSource::OcrText;
        e.name = row.text;
        e.x1 = row.x1;
        e.y1 = row.y1;
        e.x2 = row.x2;
        e.y2 = row.y2;
        out.push_back(std::move(e));
    }

    // ③ 阅读顺序（上→下、左→右）后统一编号：id 顺序必须与视觉扫描一致，
    //    否则模型说「第 3 个」和它看到的第 3 个对不上。
    std::stable_sort(out.begin(), out.end(), [](const AiElementEntry& a, const AiElementEntry& b) {
        // 纵向先按「同一视觉行」聚合（行高差 12px 内算同一行），否则小图标会被排到行首/行尾
        const int ay = a.centerY();
        const int by = b.centerY();
        if (std::abs(ay - by) > 12) return ay < by;
        return a.centerX() < b.centerX();
    });
    if (out.size() > maxItems) out.resize(maxItems);
    for (size_t i = 0; i < out.size(); ++i) out[i].id = static_cast<int>(i) + 1;
    return out;
}

std::vector<AiElementEntry> BuildAiElementIndex(
    const std::vector<AiUiAnchor>& uiControls,
    const std::vector<AiIndexRowInput>& ocrRows,
    size_t maxItems, double dedupIou) {
    return BuildAiElementIndexWithIcons(uiControls, ocrRows, {}, maxItems, dedupIou);
}

const AiElementEntry* AiElementIndexById(const std::vector<AiElementEntry>& items, int id) {
    if (id <= 0) return nullptr;
    for (const auto& e : items) {
        if (e.id != id) continue;
        // ★窗口自身按钮 / 灰控件**永不入选** —— 与按名字解析同一把尺（见头文件）。
        //   返回 nullptr 让调用方给出**可执行解释**，而不是把坐标交出去。
        if (e.windowChrome || !e.enabled) return nullptr;
        if (!e.valid()) return nullptr;
        return &e;
    }
    return nullptr;
}

// ── 「短标签 → 它标注的图标槽」：一次像素拷贝 + 一份纯判据 ────────────────────
// 判据与出处见头文件。两条实测教训写在代码旁边：
//   ① **宁可什么都不发布**：配错 = 点到别的卡（用户看到的是「乱点」），不发布只是回到
//      今天的行为（模型自己去看图）⇒ 闸门一律往紧里收。
//   ② **判据不要放在采样这一层**：第一版在这里预计算「列墨迹剖面」（与**行中位亮度**比），
//      而图标占了条带一半以上宽度时，中位数就变成图标自己的颜色 ⇒ 极性翻转、
//      检出的"块"其实是**缝**（自检立刻抓到了：合成的 4 张卡被读成 4 条缝、卡片本身 ink=0）。
//      现在这里**只拷像素**，全部判断进纯函数（能喂一段合成像素逐格自检）。
bool SampleIconBandFromBitmap(HBITMAP bmp, int capX1, int capY1,
    int sx1, int sy1, int sx2, int sy2, AiIconBand* out) {
    if (!out) return false;
    *out = AiIconBand{};
    if (!bmp) return false;
    BITMAP bm{};
    if (!GetObject(bmp, sizeof(bm), &bm) || bm.bmWidth <= 0 || bm.bmHeight <= 0) return false;
    const int bmw = static_cast<int>(bm.bmWidth);
    const int bmh = static_cast<int>(bm.bmHeight);
    const int lx1 = std::clamp(sx1 - capX1, 0, bmw);
    const int lx2 = std::clamp(sx2 - capX1, 0, bmw);
    const int ly1 = std::clamp(sy1 - capY1, 0, bmh);
    const int ly2 = std::clamp(sy2 - capY1, 0, bmh);
    const int w = lx2 - lx1;
    const int h = ly2 - ly1;
    if (w < 8 || h < 4) return false;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = bmw;
    bi.bmiHeader.biHeight = -bmh;   // top-down（与 BitmapRegionLooksLowFeature 同）
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    std::vector<uint8_t> buf(static_cast<size_t>(bmw) * static_cast<size_t>(bmh) * 4);
    HDC dc = GetDC(nullptr);
    if (!dc) return false;
    const int got = GetDIBits(dc, bmp, 0, static_cast<UINT>(bmh), buf.data(), &bi,
        DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    if (got == 0) return false;

    out->x0 = sx1;
    out->y0 = sy1;
    out->width = w;
    out->height = h;
    out->lum.assign(static_cast<size_t>(w) * static_cast<size_t>(h), 0);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t idx = (static_cast<size_t>(ly1 + y) * static_cast<size_t>(bmw)
                + static_cast<size_t>(lx1 + x)) * 4;
            const int b = buf[idx];
            const int g = buf[idx + 1];
            const int r = buf[idx + 2];
            out->lum[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)] =
                static_cast<uint8_t>((r * 299 + g * 587 + b * 114) / 1000);
        }
    }
    return true;
}

namespace {

constexpr int kIconDeviationDelta = 26;   // 与背景亮度差多少算「这里有东西」

int IconBandMedianOf(std::vector<int> v) {
    if (v.empty()) return 0;
    const size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + mid, v.end());
    return v[mid];
}

std::wstring FormatRatio2(double v) {
    const int x100 = static_cast<int>(v * 100.0 + 0.5);
    return std::to_wstring(x100 / 100) + L"." + (x100 % 100 < 10 ? L"0" : L"")
        + std::to_wstring(x100 % 100);
}

/// 一列里「偏离背景行亮度」的像素数（衡量这一列有没有东西）
int IconColumnDeviation(const AiIconBand& band, int x, const std::vector<int>& bgRow) {
    int n = 0;
    for (int y = 0; y < band.height; ++y) {
        const int bg = (y < static_cast<int>(bgRow.size())) ? bgRow[static_cast<size_t>(y)] : 128;
        if (std::abs(static_cast<int>(band.at(x, y)) - bg) > kIconDeviationDelta) ++n;
    }
    return n;
}

}  // namespace

std::wstring AiExplainIconBandSkip(int bandY1, int bandY2, int prevRowBottom,
    int capY1, int bandH) {
    // ★判据与调用点**逐字相同**：返回空就等于「这条不该拒」。
    if (!(bandY1 < capY1 || bandY2 <= bandY1)) return std::wstring();

    // 原因由谓词本身算：先看是不是「上一行标签顶到了本行条带下沿」（最挤的那种），
    // 再看是不是「为让开上一行才被顶出捕获区」（仍然是上一行的账），最后才是本行太靠上。
    if (bandY2 <= bandY1) {
        return L"条带被上一行标签压没（上一行标签底边 y " + std::to_wstring(prevRowBottom)
            + L" 已到本行条带下沿 y " + std::to_wstring(bandY2) + L"，放不下条带）";
    }
    if (bandY1 < capY1 && prevRowBottom != INT_MIN && bandY1 == prevRowBottom + 2) {
        return L"为让开上一行标签，条带上沿被顶到捕获区之外（y "
            + std::to_wstring(bandY1) + L" < 捕获区上沿 " + std::to_wstring(capY1)
            + L"，上一行标签底边 y " + std::to_wstring(prevRowBottom) + L"）";
    }
    return L"条带整体落在捕获区上沿之外（条带上沿 y " + std::to_wstring(bandY1)
        + L" 高于捕获区上沿 " + std::to_wstring(capY1) + L"，且本行标签离上沿不足条带高 "
        + std::to_wstring(bandH) + L"）";
}

std::vector<AiLabelIconPair> PairCaptionRowWithIconBand(
    const std::vector<AiIndexRowInput>& labelsIn, const AiIconBand& band, std::wstring* why) {
    std::vector<AiLabelIconPair> out;
    auto fail = [&](const std::wstring& w) {
        if (why) *why = w;
        return out;
    };
    if (why) why->clear();
    if (labelsIn.size() < 3) return fail(L"标签少于 3 段（不成排，不配对）");
    if (!band.valid() || band.width < 32 || band.height < 6) {
        return fail(L"条带太小或没采到像素");
    }
    std::vector<AiIndexRowInput> labels = labelsIn;
    std::sort(labels.begin(), labels.end(),
        [](const AiIndexRowInput& a, const AiIndexRowInput& b) { return a.x1 < b.x1; });

    // ① 短标签：成排的**短**标签才可能是图标的标注（长句是正文，配对没意义）
    long long labelHSum = 0;
    for (const auto& l : labels) {
        const size_t n = l.text.size();
        if (n == 0 || n > 6) return fail(L"标签不是短标签（≤6 字）：「" + l.text + L"」");
        labelHSum += (std::max)(1, l.y2 - l.y1);
    }
    const int labelH = (std::max)(1, static_cast<int>(labelHSum / static_cast<long long>(labels.size())));

    // ② 间距均匀（成排的图标必然等距）
    std::vector<double> gaps;
    for (size_t i = 1; i < labels.size(); ++i) {
        const int ca = (labels[i - 1].x1 + labels[i - 1].x2) / 2;
        const int cb = (labels[i].x1 + labels[i].x2) / 2;
        gaps.push_back(static_cast<double>(cb - ca));
    }
    // ★★判据的**宾语**：要判的是「这一排标签是否落在同一个等距栅格上」，
    //   而不是「这一行里每个标签的间距都一样」（docs §63）。
    //   真机代价（实测一次运行）：顶栏 12 张卡的价签**本身是等距的**，但同一视觉行里
    //   还混着**太阳计数器**（`30000`，与第一个价签只差 55px）⇒ 整行变异系数 **0.36**，
    //   只比 0.35 的门槛高 **0.01** ⇒ **整行一条都不发布**（8 次）⇒ 模型拿不到任何卡坐标
    //   ⇒ 只能反复 zoom 量像素（**17 次 zoom**，占全部动作 40%）。
    //   行里混进一个**不同来源**的标签，不该让整排作废。
    //   做法：以**中位间距**为栅格，取「间距落在中位 ±35% 内」的**最长连续段**，只在该段上判。
    //   ⚠ 仍然要求 ≥3 段 —— 只放宽「谁参与」，不放宽「成不成排」。
    {
        std::vector<double> sorted = gaps;
        std::sort(sorted.begin(), sorted.end());
        const double medianGap = sorted[sorted.size() / 2];
        if (medianGap < 8.0) {
            return fail(L"标签间距太小或重叠（中位 " + FormatRatio2(medianGap) + L"px）");
        }
        const double tol = medianGap * 0.35;
        size_t bestStart = 0, bestLen = 0;
        for (size_t i = 0; i + 1 < labels.size();) {
            size_t j = i;
            while (j + 1 < labels.size() && std::abs(gaps[j] - medianGap) <= tol) ++j;
            const size_t len = j - i + 1;   // 这一段覆盖的标签数
            if (len > bestLen) { bestLen = len; bestStart = i; }
            i = j + 1;
        }
        if (bestLen < 3) {
            return fail(L"标签间距不匀（找不到 ≥3 段的等距段；中位间距 "
                + FormatRatio2(medianGap) + L"px）");
        }
        if (bestLen < labels.size()) {
            labels = std::vector<AiIndexRowInput>(labels.begin() + static_cast<long long>(bestStart),
                labels.begin() + static_cast<long long>(bestStart + bestLen));
            gaps.assign(gaps.begin() + static_cast<long long>(bestStart),
                gaps.begin() + static_cast<long long>(bestStart + bestLen - 1));
        }
    }
    double mean = 0.0;
    for (double g : gaps) mean += g;
    mean /= static_cast<double>(gaps.size());
    double var = 0.0;
    for (double g : gaps) var += (g - mean) * (g - mean);
    var /= static_cast<double>(gaps.size());
    const double cv = std::sqrt(var) / mean;
    if (cv > 0.35) {
        return fail(L"标签间距不匀（变异系数 " + FormatRatio2(cv) + L" > 0.35）");
    }

    // ③ 背景先验：从**标签之间**（以及条带两端）的窗口按行取中位亮度。
    //    为什么要用「标签之间」而不是整行中位数：图标占条带一半以上宽度时，整行中位数
    //    就是图标自己的颜色 ⇒ 判据极性翻转、把缝当块（第一版就是这么错的）。
    //    而"标签之间是背景"本来就是这条推断的**假设**，用假设本身来定背景是自洽的。
    const int win = (std::max)(2, static_cast<int>(mean * 0.06));
    std::vector<int> bgCols;
    auto addBgWindow = [&](int screenX) {
        for (int x = screenX - win; x <= screenX + win; ++x) {
            const int lx = x - band.x0;
            if (lx >= 0 && lx < band.width) bgCols.push_back(lx);
        }
    };
    for (size_t i = 0; i + 1 < labels.size(); ++i) {
        const int ca = (labels[i].x1 + labels[i].x2) / 2;
        const int cb = (labels[i + 1].x1 + labels[i + 1].x2) / 2;
        addBgWindow((ca + cb) / 2);
    }
    if (bgCols.empty()) return fail(L"标签之间没有空隙可看（取不到背景样本）");
    std::vector<int> bgRow(static_cast<size_t>(band.height), 128);
    std::vector<int> samples;
    samples.reserve(bgCols.size());
    for (int y = 0; y < band.height; ++y) {
        samples.clear();
        for (int lx : bgCols) samples.push_back(band.at(lx, y));
        const size_t mid = samples.size() / 2;
        std::nth_element(samples.begin(), samples.begin() + mid, samples.end());
        bgRow[static_cast<size_t>(y)] = samples[mid];
    }

    // ④ 每段标签的**正上方**必须确有东西（列偏离量的中位数要够大）
    const int minCenterInk = (std::max)(3, band.height / 4);
    std::vector<int> centerInk(labels.size(), 0);
    for (size_t i = 0; i < labels.size(); ++i) {
        const int cx = (labels[i].x1 + labels[i].x2) / 2 - band.x0;
        std::vector<int> v;
        for (int x = cx - win; x <= cx + win; ++x) {
            if (x < 0 || x >= band.width) continue;
            v.push_back(IconColumnDeviation(band, x, bgRow));
        }
        if (v.empty()) {
            return fail(L"第 " + std::to_wstring(i + 1) + L" 段标签上方取不到列");
        }
        centerInk[i] = IconBandMedianOf(v);
        if (centerInk[i] < minCenterInk) {
            return fail(L"第 " + std::to_wstring(i + 1) + L" 段标签正上方没有独立的东西"
                L"（列偏离中位数 " + std::to_wstring(centerInk[i]) + L" < 需要 "
                + std::to_wstring(minCenterInk) + L"）");
        }
    }

    // ⑤ 每块横向的**范围**：先按「相邻两个标签的中点」划出这一格（相邻格天然不重叠），
    //    再在格内取「脱离背景」的那段连续列 —— 既不会串到邻居，也不会把背景算进去。
    //    ⚠ 第一版是「从标签心向两边扩，扩到偏离掉一半就停；扩到邻居的心就整行作废」——
    //    在**紧凑网格**里（卡片之间只隔一条深色描边、不是背景色）它必然扩到邻居
    //    ⇒ 整行作废。而「一排图标＋下面一排短标签」最常见的形状就是网格，不能因为
    //    「分不开」就整行放弃：中点本来就是那一格边界的最佳估计。
    std::vector<AiLabelIconPair> pairs;
    std::vector<int> centers(labels.size(), 0);
    for (size_t i = 0; i < labels.size(); ++i) {
        centers[i] = (labels[i].x1 + labels[i].x2) / 2 - band.x0;
    }
    for (size_t i = 0; i < labels.size(); ++i) {
        const int cx = centers[i];
        const int leftBound = (i > 0) ? (centers[i - 1] + cx) / 2 : 0;
        const int rightBound = (i + 1 < labels.size())
            ? (cx + centers[i + 1]) / 2 : band.width - 1;
        const int halfInk = (std::max)(1, centerInk[i] / 2);
        if (IconColumnDeviation(band, cx, bgRow) < halfInk) {
            return fail(L"第 " + std::to_wstring(i + 1) + L" 段标签正上方那一列不像图标");
        }
        int lo = cx;
        int hi = cx;
        while (lo - 1 >= leftBound && IconColumnDeviation(band, lo - 1, bgRow) >= halfInk) --lo;
        while (hi + 1 <= rightBound && IconColumnDeviation(band, hi + 1, bgRow) >= halfInk) ++hi;
        // 只有**顶到条带外沿**才算被切了（格边界顶到不算 —— 那正是"这一格被填满"）
        if (lo <= 0 || hi + 1 >= band.width) {
            return fail(L"最边上的那块顶到条带边缘（被切了，中心不可信）");
        }
        const int slotW = hi - lo + 1;
        if (slotW * 10 < static_cast<int>(mean * 3.0)) {
            return fail(L"第 " + std::to_wstring(i + 1) + L" 块只有 " + std::to_wstring(slotW)
                + L" 列（< 0.3×标签间距）—— 不像图标");
        }
        // ⑥ 块高：块内「多数列都偏离背景」的行才算块的一部分。
        //    实测价值：**上面那行是另一行文字**时，它只有一行那么高 ⇒ 被下面这条挡掉。
        int top = -1;
        int bottom = -1;
        for (int y = 0; y < band.height; ++y) {
            int dev = 0;
            for (int x = lo; x <= hi; ++x) {
                if (std::abs(static_cast<int>(band.at(x, y)) - bgRow[static_cast<size_t>(y)])
                    > kIconDeviationDelta) {
                    ++dev;
                }
            }
            if ((hi - lo + 1) > 0 && dev * 100 >= (hi - lo + 1) * 35) {
                if (top < 0) top = y;
                bottom = y;
            }
        }
        if (top < 0) return fail(L"第 " + std::to_wstring(i + 1) + L" 块里找不到成片的行");
        const int ih = bottom - top + 1;
        if (ih * 10 < labelH * 16) {
            return fail(L"第 " + std::to_wstring(i + 1) + L" 块高 " + std::to_wstring(ih)
                + L"px < 1.6×标签高 " + std::to_wstring(labelH) + L"px（像文字行，不是图标）");
        }
        AiLabelIconPair p;
        p.label = labels[i];
        p.x1 = band.x0 + lo;
        p.x2 = band.x0 + hi + 1;
        p.y1 = band.y0 + top;
        p.y2 = band.y0 + bottom + 1;
        p.slotCount = static_cast<int>(labels.size());
        p.labelCount = static_cast<int>(labels.size());
        p.iconHeight = ih;
        pairs.push_back(p);
    }

    if (why) why->clear();
    return pairs;
}

std::wstring FormatAiElementIndex(const std::vector<AiElementEntry>& items,
    int mapCapX1, int mapCapY1, int mapCapX2, int mapCapY2, int apiW, int apiH,
    size_t maxChars) {
    if (items.empty()) return {};
    if (maxChars == 0) maxChars = 2600;
    const int regionW = (std::max)(1, mapCapX2 - mapCapX1);
    const int regionH = (std::max)(1, mapCapY2 - mapCapY1);
    const int frameW = (std::max)(1, apiW);
    const int frameH = (std::max)(1, apiH);
    auto toFrameX = [&](int screenX) {
        return std::clamp(static_cast<int>(
            static_cast<double>(screenX - mapCapX1) * frameW / regionW), 0, frameW - 1);
    };
    auto toFrameY = [&](int screenY) {
        return std::clamp(static_cast<int>(
            static_cast<double>(screenY - mapCapY1) * frameH / regionH), 0, frameH - 1);
    };

    std::wstring out = L"元素索引（本帧一次枚举出来的「看得见的东西」：UIA 控件 ∪ OCR 文字）。"
        L"⚠ **不是每条都能点**：标「控件」的来自系统可访问性树（通常可交互）；"
        L"标「文字」的只是**这一帧在屏幕上读到的字** —— 模式标签、标题、计数器、说明文字"
        L"都会出现在这里，**可点性未经验证**。实测有模型去点「模式标签」和「面板标题」"
        L"（点了没反应，白费一轮）⇒ 点了没反应就换目标，别对同一条反复点。"
        L"★坐标为 **upload 截图像素**（和 mouseClick(x,y) 完全同一套），"
        L"图像尺寸 " + std::to_wstring(frameW) + L"×" + std::to_wstring(frameH)
        + L"。要按坐标点就直接把这个 (x,y) 填进 mouseClick；"
          L"要按名字点就调 locateAndClick(target=\"名字\")——"
          L"名字能在索引里查到就是 0 次识图。\n"
        // ★★编号可用（docs §32.4 缺的「最后一公里」）：此前索引给了编号却没有任何工具吃得下它，
        //   模型只能写**名字**去匹配，而同屏同名条目会判歧义 ⇒ 白回落一整轮识图。
          L"★★也可以**按编号点**：locateAndClick(elementId=N) 直接点这条（下面是 [N]），"
          L"同屏多个同名条目时按编号不会判歧义。编号**只在当前这一帧有效**，"
          L"界面变了请用新的清单编号。\n"
          L"每条还带角色与能力（如「滑块 action:slide」「输入框 action:fill」）"
          L"以及本地读到的状态（[value:…] [range:…] [toggle:…] [focused] "
          L"[state:expanded] [v:…%]）——**这些是画面/截图上读不出来的事实**，"
          L"据此可以少点一次「先点一下看看」。"
          L"标「文字」的来源没有能力与状态（本地没有证据，不猜）。\n";
    for (const auto& e : items) {
        if (e.windowChrome) continue;   // 窗口自身按钮不进清单（列出来只会诱导模型去点）
        const int fx = toFrameX(e.centerX());
        const int fy = toFrameY(e.centerY());
        std::wstring line = L"[" + std::to_wstring(e.id) + L"] " + AiElementSourceName(e.source)
            + L" \"" + e.name + L"\" (" + std::to_wstring(fx) + L","
            + std::to_wstring(fy) + L")";
        // ★角色 + 动作能力：回答「这是什么、支持哪一类操作」——**事实**，不是建议。
        //   没有它，模型看不出「某个条目是能填值的输入框、还是要点一下的按钮」，
        //   只能先截图看一眼或先点一下试试（一次动作 + 一次观察就这么白烧掉）。
        if (!e.role.empty()) {
            line += L" " + e.role;
        }
        if (!e.action.empty()) {
            line += L" action:" + e.action;
        }
        if (!e.enabled) line += L"（灰·点了没用）";
        if (e.invokable) line += L"[可直接触发]";
        // ★可读状态事实：focused / value:"…" / range:0-100 / toggle:on / state:expanded /
        //   v:42% / readonly / password(值不回传)。这些在画面上读不出来或极易读错。
        for (const auto& s : e.state) {
            line += L" [" + s + L"]";
        }
        if (!e.windowTitle.empty()) line += L" 窗口「" + e.windowTitle + L"」";
        // 推断类条目（图标槽）必须把**依据**一起给：模型才有机会自己判断该不该信。
        if (!e.note.empty()) line += L"\n    ↑ " + e.note;
        line += L"\n";
        if (out.size() + line.size() > maxChars) {
            out += L"…(索引过长已截断)\n";
            break;
        }
        out += line;
    }
    return out;
}

bool AiElementIndexResolve(const std::vector<AiElementEntry>& items,
    const std::wstring& targetDesc, bool hasNear, int nearX, int nearY,
    AiIndexResolveResult* out) {
    AiIndexResolveResult r;
    const std::wstring normTarget = IndexNormTarget(targetDesc);
    if (normTarget.empty()) {
        r.why = L"目标为空";
        if (out) *out = r;
        return false;
    }
    // ⚠ 短目标/纯数字（「1」「600」）**只允许精确档**，不许宽容档：
    //   满屏都是这种短标签，模糊匹配必然点到别处；但表格单元格、卡片角标的价格
    //   本来就是纯数字，一律拒绝又会让「按价格/编号选」彻底不可用。
    //   所以按档位收紧（而不是按目标形状一刀切）：命中要"完全相等"才算数。
    const bool strictTierOnly = IndexTargetTooAmbiguous(normTarget);

    // 档位：2 完全相等，1 双向包含（与文字直点同一把尺，见 AiOcrLabelMatchTier）
    struct Hit {
        const AiElementEntry* e = nullptr;
        int tier = 0;
        long long d2 = 0;
        bool fuzzy = false;
    };
    std::vector<Hit> hits;
    bool sawChromeMatch = false;
    bool sawTier2 = false;
    for (const auto& e : items) {
        if (e.windowChrome) {
            // ★窗口自身按钮永不入选。记一笔，好在「只匹配到它」时给出可执行的解释，
            //   而不是笼统的「索引里没有」——这正是把整个游戏窗口关掉的那条路。
            if (AiOcrLabelMatchTier(e.name, targetDesc) > 0) sawChromeMatch = true;
            continue;
        }
        if (!e.enabled) continue;
        int tier = AiOcrLabelMatchTier(e.name, targetDesc);
        bool fuzzy = false;
        // 精确/包含档都不中时，再试**宽容档**（CJK 短标签丢字/并字）——
        // OCR 把「自选僵尸卡牌」读成「自选僵尸卡」这类，长度比闸会把它挡掉，
        // 于是模型在索引里查不到自己刚看到的文字，反复识图 + 反复推敲（实测一轮 40s+）。
        if (tier <= 0 && !strictTierOnly && AiElementIndexPartialMatch(e.name, targetDesc) > 0) {
            tier = 1;
            fuzzy = true;
        }
        if (tier <= 0) continue;
        if (tier >= 2) sawTier2 = true;
        Hit h;
        h.e = &e;
        h.tier = tier;
        h.fuzzy = fuzzy;
        const long long dx = e.centerX() - nearX;
        const long long dy = e.centerY() - nearY;
        h.d2 = dx * dx + dy * dy;
        hits.push_back(h);
    }
    if (hits.empty()) {
        r.why = sawChromeMatch
            ? L"「" + targetDesc + L"」只匹配到**窗口自身的标题栏按钮**（关/最小化/最大化），"
              L"不是应用里的控件 —— 已拒绝，按名字点它会把整个窗口关掉"
            : L"元素索引里没有「" + targetDesc + L"」";
        if (out) *out = r;
        return false;
    }
    // 有精确/包含档命中时，宽容档的候选直接丢掉（不许模糊的抢走精确的）
    if (sawTier2) {
        std::vector<Hit> keep;
        for (const Hit& h : hits) {
            if (h.tier >= 2) keep.push_back(h);
        }
        hits.swap(keep);
    }

    // 最高档位优先
    int bestTier = 0;
    for (const Hit& h : hits) bestTier = (std::max)(bestTier, h.tier);
    std::vector<Hit> top;
    for (const Hit& h : hits) {
        if (h.tier == bestTier) top.push_back(h);
    }

    if (top.size() > 1) {
        // 同档多条，先按「来源可信度」收敛：同一处既可能来自 OCR 文字（文字框中心，
        // 常偏在卡片角标上），也可能来自 UIA 控件（控件真框，才贴那个可点目标），
        // 还可能来自「标签→图标槽」推断（那是**图标本体**的框，比价签文字更贴目标）。
        // 只要存在更高优先级的来源，就只留它 —— 这不是"猜"，是同一处的更准表示。
        auto sourcePref = [](AiElementSource s) {
            switch (s) {
            case AiElementSource::UiAutomation: return 2;
            case AiElementSource::LabeledIcon: return 1;
            default: return 0;
            }
        };
        int bestPref = 0;
        for (const Hit& h : top) bestPref = (std::max)(bestPref, sourcePref(h.e->source));
        if (bestPref > 0) {
            std::vector<Hit> keep;
            for (const Hit& h : top) {
                if (sourcePref(h.e->source) == bestPref) keep.push_back(h);
            }
            top.swap(keep);
        }
    }
    if (top.size() > 1) {
        // 同档多条：有 near 就取最近的那条（识图点/上一步通常已很接近目标）；
        // 没 near 或最近的两条几乎一样近 → 歧义，宁可回落识图也不猜着点。
        if (!hasNear) {
            r.ambiguous = true;
            r.why = L"索引里有 " + std::to_wstring(top.size()) + L" 条同名「" + targetDesc
                + L"」，且没有位置线索可区分 → 拒绝（请给更具体的描述，或用带 near 的调用）";
            if (out) *out = r;
            return false;
        }
        std::stable_sort(top.begin(), top.end(),
            [](const Hit& a, const Hit& b) { return a.d2 < b.d2; });
        // 最近两条距离差 < 24px：同样近 = 分不出，判歧义
        if (top.size() >= 2) {
            const long long d0 = top[0].d2;
            const long long d1 = top[1].d2;
            if (d1 - d0 < 24LL * 24LL) {
                r.ambiguous = true;
                r.why = L"索引里有 " + std::to_wstring(top.size()) + L" 条同名「" + targetDesc
                    + L"」且位置相近，无法区分 → 拒绝";
                if (out) *out = r;
                return false;
            }
        }
    }

    const AiElementEntry& e = *top.front().e;
    r.ok = true;
    r.x = e.centerX();
    r.y = e.centerY();
    r.id = e.id;
    r.name = e.name;
    r.source = e.source;
    r.tier = top.front().tier;
    if (out) *out = r;
    return true;
}
