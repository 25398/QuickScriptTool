#include "ai_locate_verify.h"

#include "utils.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <functional>

namespace {

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

    bool havePick = false;
    OcrTextLine best{};
    const wchar_t* pickKind = L"";
    std::wstring fallbackWhy = L"OCR 索引里没有「" + wantText + L"」";
    for (int t = 0; t < 2 && !havePick; ++t) {
        if (tiers[t].empty()) continue;
        const std::vector<OcrTextLine>& sane = tiers[t];
        // 唯一性：同一档里出现多个相距较远的候选 = 同屏多个同名按钮 → 歧义
        const int cx0 = (sane[0].x1 + sane[0].x2) / 2;
        const int cy0 = (sane[0].y1 + sane[0].y2) / 2;
        bool ambiguous = false;
        for (size_t i = 1; i < sane.size(); ++i) {
            const int cx = (sane[i].x1 + sane[i].x2) / 2;
            const int cy = (sane[i].y1 + sane[i].y2) / 2;
            if (std::abs(cx - cx0) > 16 || std::abs(cy - cy0) > 16) {
                ambiguous = true;
                break;
            }
        }
        if (ambiguous) {
            fallbackWhy = L"OCR 同屏有多个「" + wantText + L"」（"
                + std::to_wstring(sane.size()) + L" 处），歧义不做直点";
            continue;
        }
        best = sane[0];
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
