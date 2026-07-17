// ──────────────────────────────────────────────────────────────────
// ui_component.cpp — 统一 UI 组件抽象系统实现
// ──────────────────────────────────────────────────────────────────
#include "ui_component.h"

#include <algorithm>

#include "config.h"
#include "controls.h"
#include "modern_edit.h"
#include "ui_scale.h"

// ══════════════════════════════════════════════════════════════════
// 缩放辅助函数 (基于 1024->1200 设计稿比例)
// ══════════════════════════════════════════════════════════════════

int ScaleX(int baseX) {
    return MulDiv(baseX, UiEditorWidth(), kEditorBaseWidth);
}
int ScaleY(int baseY) {
    return MulDiv(baseY, UiEditorHeight(), kEditorBaseHeight);
}
int ScaleW(int baseW) {
    return MulDiv(baseW, UiEditorWidth(), kEditorBaseWidth);
}
int ScaleH(int baseH) {
    return MulDiv(baseH, UiEditorHeight(), kEditorBaseHeight);
}

// ══════════════════════════════════════════════════════════════════
// 样式预设工厂
// ══════════════════════════════════════════════════════════════════

namespace StylePreset {

UIStyle EditorLabel() {
    UIStyle s;
    s.textColor = kText;
    s.font.height = 14;
    s.textAlign = DT_LEFT | DT_TOP;
    s.paddingTop = 2;
    return s;
}

UIStyle NormalLabel() {
    UIStyle s;
    s.textColor = kText;
    s.font.height = 14;
    s.textAlign = DT_LEFT | DT_VCENTER | DT_SINGLELINE;
    return s;
}

UIStyle EditField() {
    UIStyle s;
    s.textColor = kText;
    s.bgColor = kWhite;
    s.borderColor = kComboBorderGray;
    s.font.height = 14;
    return s;
}

UIStyle NormalButton() {
    UIStyle s;
    s.font.height = 14;
    return s;
}

UIStyle ActionGreenButton() {
    UIStyle s;
    s.textColor = kWhite;
    s.bgColor = kButtonGreen;
    s.font.height = 14;
    return s;
}

UIStyle ActionGrayButton() {
    UIStyle s;
    s.textColor = kGrayButtonText;
    s.bgColor = kGrayButton;
    s.borderColor = kGrayButtonBorder;
    s.font.height = 13;
    return s;
}

UIStyle HintText() {
    UIStyle s;
    s.textColor = kHint;
    s.font.height = 12;
    s.textAlign = DT_LEFT | DT_WORDBREAK;
    return s;
}

UIStyle NormalCheckBox() {
    UIStyle s;
    s.textColor = kText;
    s.font.height = 14;
    return s;
}

}  // namespace StylePreset

// ══════════════════════════════════════════════════════════════════
// 默认组件尺寸常量 (基于设计稿 1024×768)
// ══════════════════════════════════════════════════════════════════

namespace {
constexpr int kDefaultLabelH = 22;
constexpr int kDefaultEditH = 22;
constexpr int kDefaultButtonH = 28;
constexpr int kDefaultComboH = 21;
constexpr int kDefaultCheckBoxH = 25;
constexpr int kDefaultHintH = 40;
constexpr int kDefaultCaptureH = 21;
constexpr int kDefaultMultilineEditH = 60;

int DefaultHeightForType(UIComponentType type) {
    switch (type) {
    case UIComponentType::Label:       return kDefaultLabelH;
    case UIComponentType::EditorLabel: return kDefaultLabelH;
    case UIComponentType::Hint:        return kDefaultHintH;
    case UIComponentType::Edit:        return kDefaultEditH;
    case UIComponentType::FieldEdit:   return kDefaultEditH;
    case UIComponentType::MultilineEdit: return kDefaultMultilineEditH;
    case UIComponentType::Button:      return kDefaultButtonH;
    case UIComponentType::GreenButton: return kDefaultButtonH;
    case UIComponentType::GrayButton:  return kDefaultButtonH;
    case UIComponentType::CaptureField:return kDefaultCaptureH;
    case UIComponentType::CheckBox:    return kDefaultCheckBoxH;
    case UIComponentType::ComboLabel:  return kDefaultComboH;
    case UIComponentType::Custom:      return 0;
    case UIComponentType::Spacer:      return 0;
    }
    return kDefaultLabelH;
}
}  // namespace

// ══════════════════════════════════════════════════════════════════
// 核心布局计算
// ══════════════════════════════════════════════════════════════════

UILayoutResult CalculateLayout(const UILayout& layout, bool scaled) {
    UILayoutResult result;
    if (layout.rows.empty()) return result;

    // 确定布局起始位置 (缩放后的坐标)
    const int left = scaled ? ScaleX(layout.left) : layout.left;
    const int startY = scaled ? ScaleY(layout.top) : layout.top;
    const int regionW = scaled ? ScaleW(layout.width) : layout.width;
    const int rowGap = scaled ? ScaleY(layout.rowGap) : layout.rowGap;

    int currentY = startY;
    int maxRight = left;

    for (const auto& row : layout.rows) {
        if (row.components.empty()) {
            // 空行作为间距
            currentY += row.marginTop + row.marginBottom;
            continue;
        }

        // 行内不可填充组件数量 (width > 0 的组件)
        int fixedCount = 0;
        int totalFixedW = 0;
        for (const auto& c : row.components) {
            if (c.width > 0) {
                ++fixedCount;
                totalFixedW += scaled ? ScaleW(c.width) : c.width;
            }
        }
        const int fillCount = static_cast<int>(row.components.size()) - fixedCount;
        const int gapX = scaled ? ScaleX(row.gapX) : row.gapX;
        const int totalGaps = gapX * static_cast<int>(std::max(0, static_cast<int>(row.components.size()) - 1));
        const int fillW = fillCount > 0
            ? (regionW - totalFixedW - totalGaps) / fillCount
            : 0;

        // 计算行的最高组件高度
        int rowH = 0;
        for (const auto& c : row.components) {
            int h = c.height > 0 ? (scaled ? ScaleH(c.height) : c.height) : DefaultHeightForType(c.type);
            rowH = std::max(rowH, h);
        }

        // 应用行外边距
        currentY += scaled ? ScaleY(row.marginTop) : row.marginTop;

        // 放置行内组件
        int currentX = left;
        bool prevWasNonSpacer = false;  // 上一个组件是否为非 Spacer 类型
        for (size_t ci = 0; ci < row.components.size(); ++ci) {
            const auto& comp = row.components[ci];
            if (comp.type == UIComponentType::Spacer) {
                int w = comp.width > 0 ? (scaled ? ScaleW(comp.width) : comp.width) : 0;
                // Spacer 自身即为间距，不需要额外加 gapX
                currentX += w;
                prevWasNonSpacer = false;
                continue;
            }

            int w = comp.width > 0 ? (scaled ? ScaleW(comp.width) : comp.width) : fillW;
            int h = comp.height > 0 ? (scaled ? ScaleH(comp.height) : comp.height) : DefaultHeightForType(comp.type);
            // 正方形预览按钮（120×120）统一按宽比缩放
            if (comp.type == UIComponentType::GrayButton && comp.width > 0
                && comp.width == comp.height && comp.text.empty()) {
                const int s = scaled ? ScaleW(comp.width) : comp.width;
                w = h = s;
            }

            // 确定层数: 行级 > 组件级 > 布局默认
            int layer = layout.defaultLayer;
            if (comp.style.layer != 0) layer = comp.style.layer;
            if (row.layer != 0) layer = row.layer;

            // 仅在两个非 Spacer 组件之间添加 gapX
            if (prevWasNonSpacer) currentX += gapX;
            const int layoutRight = left + regionW;
            if (currentX < layoutRight && currentX + w > layoutRight) {
                w = std::max(1, layoutRight - currentX);
            }

            UIComponentPlacement placement;
            placement.type = comp.type;
            placement.x = currentX;
            placement.y = currentY;
            placement.width = w;
            placement.height = h;
            placement.layer = layer;
            placement.id = comp.id;
            result.placements.push_back(placement);

            currentX += w;
            maxRight = std::max(maxRight, currentX);
            prevWasNonSpacer = true;
        }

        currentY += rowH;
        currentY += scaled ? ScaleY(row.marginBottom) : row.marginBottom;
        currentY += rowGap;
    }

    result.totalHeight = currentY - startY;
    result.totalWidth = maxRight - left;
    return result;
}

// ══════════════════════════════════════════════════════════════════
// 单个组件创建
// ══════════════════════════════════════════════════════════════════

HWND CreateComponent(HWND parent, const UIComponent& comp, int x, int y) {
    switch (comp.type) {
    case UIComponentType::Label:
        return MakeLabel(parent, comp.text.c_str(), comp.id, x, y,
            comp.width, comp.height);
    case UIComponentType::EditorLabel:
        return MakeEditorLabel(parent, comp.text.c_str(), comp.id, x, y,
            comp.width, comp.height);
    case UIComponentType::Hint:
        return MakeHint(parent, comp.text.c_str(), x, y,
            comp.width, comp.height);
    case UIComponentType::Edit:
        return MakeEdit(parent, comp.text.c_str(), comp.id, x, y,
            comp.width, comp.height);
    case UIComponentType::FieldEdit:
        return MakeFieldEdit(parent, comp.text.c_str(), comp.id, x, y,
            comp.width, comp.height);
    case UIComponentType::MultilineEdit:
        return MakeMultilineEdit(parent, comp.text.c_str(), comp.id, x, y,
            comp.width, comp.height);
    case UIComponentType::Button:
        return MakeButton(parent, comp.text.c_str(), comp.id, x, y,
            comp.width, comp.height);
    case UIComponentType::GreenButton:
        return MakeGreenButton(parent, comp.text.c_str(), comp.id, x, y,
            comp.width, comp.height);
    case UIComponentType::GrayButton:
        return MakeGrayButton(parent, comp.text.c_str(), comp.id, x, y,
            comp.width, comp.height);
    case UIComponentType::CaptureField:
        return MakeCaptureField(parent, comp.text.c_str(), comp.id, x, y,
            comp.width, comp.height);
    case UIComponentType::CheckBox:
        return MakeCheckBox(parent, comp.text.c_str(), comp.id, x, y,
            comp.width, comp.height);
    case UIComponentType::ComboLabel:
        return MakeLabel(parent, comp.text.c_str(), comp.id, x, y,
            comp.width, comp.height);
    case UIComponentType::Custom:
        if (comp.existingHwnd) {
            SetWindowPos(comp.existingHwnd, nullptr, x, y,
                comp.width, comp.height, SWP_NOZORDER | SWP_NOACTIVATE);
            ShowWindow(comp.existingHwnd, SW_SHOW);
            return comp.existingHwnd;
        }
        return nullptr;
    case UIComponentType::Spacer:
        return nullptr;
    }
    return nullptr;
}

// ══════════════════════════════════════════════════════════════════
// 布局构建 (计算 + 创建控件)
// ══════════════════════════════════════════════════════════════════

UILayoutResult BuildLayout(HWND parent, const UILayout& layout, bool scaled) {
    // scaled=false: 返回设计坐标，由 Make* 函数内部执行缩放（与原代码一致）
    // scaled=true:  返回屏幕坐标，适用于不调用 Make* 函数的场景
    UILayoutResult result = CalculateLayout(layout, scaled);

    if (layout.rows.empty()) return result;

    // 确定布局起始位置用于组件创建 (组件坐标需用布局内的绝对坐标)
    // CalculateLayout 已经使用了缩放的 left/top，不需要再次偏移

    int compIdx = 0;

    for (const auto& row : layout.rows) {
        for (size_t ci = 0; ci < row.components.size(); ++ci) {
            const auto& comp = row.components[ci];
            if (comp.type == UIComponentType::Spacer) continue;

            auto& placement = result.placements[static_cast<size_t>(compIdx)];
            UIComponent actual = comp;
            actual.width = placement.width;
            actual.height = placement.height;
            placement.hwnd = CreateComponent(parent, actual, placement.x, placement.y);
            ++compIdx;
        }
    }

    return result;
}

// ══════════════════════════════════════════════════════════════════
// 布局组显示/隐藏
// ══════════════════════════════════════════════════════════════════

void ShowLayoutGroup(const UILayoutResult& result, bool visible) {
    for (const auto& p : result.placements) {
        if (p.hwnd) {
            ShowWindow(p.hwnd, visible ? SW_SHOW : SW_HIDE);
        }
    }
}
