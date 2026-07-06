// ──────────────────────────────────────────────────────────────────
// ui_component.h — 统一 UI 组件抽象系统
//
// 设计目标：
// 1. 拼接模式顺序排列 — Row/Col 上下左右依次堆叠，自动计算坐标
// 2. 统一样式定义 — UIStyle 集中管理颜色、字体、间距等视觉属性
// 3. 层数控制 — layer 参数决定 GDI 绘制顺序 (值越大越靠上)
// ──────────────────────────────────────────────────────────────────
#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <variant>
#include <vector>

// ══════════════════════════════════════════════════════════════════
// 1. 组件类型枚举
// ══════════════════════════════════════════════════════════════════

enum class UIComponentType {
    Label,           // 普通文本标签
    EditorLabel,     // 编辑器参数区标题标签
    Hint,            // 多行提示文本
    Edit,            // 单行编辑框
    FieldEdit,       // 无边框单行编辑框 (父窗口绘制边框)
    MultilineEdit,   // 多行编辑框
    Button,          // 标准按钮
    GreenButton,     // 自绘绿色按钮
    GrayButton,      // 自绘灰色按钮
    CaptureField,    // 按键捕获字段
    CheckBox,        // 复选框
    ComboLabel,      // 下拉组合框 (基于 Label)
    Custom,          // 自定义控件 (外部已创建的 HWND)
    Spacer,          // 空白占位 (仅影响布局，不创建 HWND)
};

// ══════════════════════════════════════════════════════════════════
// 2. 样式定义
// ══════════════════════════════════════════════════════════════════

struct UIFont {
    int height = 16;
    int weight = FW_NORMAL;
    bool italic = false;
    const wchar_t* faceName = L"Microsoft YaHei";
};

struct UIStyle {
    // 文本样式
    COLORREF textColor = RGB(28, 28, 28);
    COLORREF bgColor = RGB(255, 255, 255);
    UIFont font;                        // 字体定义

    // 边框样式
    COLORREF borderColor = RGB(204, 204, 204);
    int borderWidth = 1;
    int borderRadius = 0;

    // 间距样式
    int paddingTop = 0;
    int paddingBottom = 0;
    int paddingLeft = 0;
    int paddingRight = 0;

    // 对齐
    UINT textAlign = DT_LEFT | DT_VCENTER | DT_SINGLELINE;

    // 层数 (GDI 绘制顺序，值越大越靠上)
    int layer = 0;
};

// ══════════════════════════════════════════════════════════════════
// 3. 组件定义
// ══════════════════════════════════════════════════════════════════

struct UIComponent {
    UIComponentType type = UIComponentType::Label;
    int id = -1;                        // 控件 ID (用于 WM_COMMAND)
    std::wstring text;                  // 显示文本 / 初始值
    int width = 0;                      // 组件宽度 (0 = 自动填充行宽)
    int height = 0;                     // 组件高度 (0 = 类型默认高度)
    UIStyle style;                      // 样式定义
    HWND existingHwnd = nullptr;        // Custom 类型时使用的外部 HWND

    // 简化构造辅助
    static UIComponent Label(const wchar_t* text, int id, int w, int h,
        UIStyle s = {}) {
        UIComponent c{UIComponentType::Label, id, text, w, h, s};
        return c;
    }
    static UIComponent EditorLabel(const wchar_t* text, int id, int w, int h,
        UIStyle s = {}) {
        UIComponent c{UIComponentType::EditorLabel, id, text, w, h, s};
        return c;
    }
    static UIComponent Hint(const wchar_t* text, int w, int h,
        UIStyle s = {}) {
        UIComponent c{UIComponentType::Hint, -1, text, w, h, s};
        return c;
    }
    static UIComponent Edit(const wchar_t* text, int id, int w, int h,
        UIStyle s = {}) {
        UIComponent c{UIComponentType::Edit, id, text, w, h, s};
        return c;
    }
    static UIComponent Button(const wchar_t* text, int id, int w, int h,
        UIStyle s = {}) {
        UIComponent c{UIComponentType::Button, id, text, w, h, s};
        return c;
    }
    static UIComponent GreenButton(const wchar_t* text, int id, int w, int h,
        UIStyle s = {}) {
        UIComponent c{UIComponentType::GreenButton, id, text, w, h, s};
        return c;
    }
    static UIComponent GrayButton(const wchar_t* text, int id, int w, int h,
        UIStyle s = {}) {
        UIComponent c{UIComponentType::GrayButton, id, text, w, h, s};
        return c;
    }
    static UIComponent Combo(const wchar_t* text, int id, int w, int h,
        UIStyle s = {}) {
        UIComponent c{UIComponentType::ComboLabel, id, text, w, h, s};
        return c;
    }
    static UIComponent CheckBox(const wchar_t* text, int id, int w, int h,
        UIStyle s = {}) {
        UIComponent c{UIComponentType::CheckBox, id, text, w, h, s};
        return c;
    }
    static UIComponent MultilineEdit(const wchar_t* text, int id, int w, int h,
        UIStyle s = {}) {
        UIComponent c{UIComponentType::MultilineEdit, id, text, w, h, s};
        return c;
    }
    static UIComponent CaptureField(const wchar_t* text, int id, int w, int h,
        UIStyle s = {}) {
        UIComponent c{UIComponentType::CaptureField, id, text, w, h, s};
        return c;
    }
    static UIComponent FieldEdit(const wchar_t* text, int id, int w, int h,
        UIStyle s = {}) {
        UIComponent c{UIComponentType::FieldEdit, id, text, w, h, s};
        return c;
    }
    static UIComponent Spacer(int w, int h) {
        UIComponent c{UIComponentType::Spacer, -1, L"", w, h};
        return c;
    }
    static UIComponent Custom(HWND hwnd, int w, int h,
        UIStyle s = {}) {
        UIComponent c{UIComponentType::Custom, -1, L"", w, h, s};
        c.existingHwnd = hwnd;
        return c;
    }
};

// ══════════════════════════════════════════════════════════════════
// 4. 行定义 — 一行可包含多个水平排列的组件
// ══════════════════════════════════════════════════════════════════

struct UIRow {
    std::vector<UIComponent> components; // 本行包含的组件 (从左到右)
    int gapX = 6;                        // 行内组件水平间距
    int marginTop = 4;                   // 行上方外边距
    int marginBottom = 4;                // 行下方外边距
    UIStyle rowStyle;                    // 行级样式 (可覆盖层数等)
    int layer = 0;                       // 行层数 (优先级高于组件级)

    UIRow() = default;
    UIRow(std::vector<UIComponent> cs, int g = 6, int mt = 4, int mb = 4)
        : components(std::move(cs)), gapX(g), marginTop(mt), marginBottom(mb) {}

    static UIRow Single(UIComponent c) {
        UIRow row;
        row.components.push_back(std::move(c));
        return row;
    }
};

// ══════════════════════════════════════════════════════════════════
// 5. 布局定义 — 整个参数面板的布局描述
// ══════════════════════════════════════════════════════════════════

struct UILayout {
    std::vector<UIRow> rows;             // 行序列 (从上到下)
    int rowGap = 8;                      // 行间距
    int left = 0;                        // 布局区域左上 X
    int top = 0;                         // 布局区域左上 Y
    int width = 0;                       // 布局区域宽度
    int defaultLayer = 0;                // 默认层数
    int groupIndex = -1;                 // 控制组索引 (用于显示/隐藏)

    // 便捷构造: 直接传入行列表
    UILayout(std::vector<UIRow> rs = {}) : rows(std::move(rs)) {}
    // 便捷构造: 指定面板位置和尺寸
    UILayout(int l, int t, int w, int g = 0, int layer = 0)
        : rowGap(g), left(l), top(t), width(w), defaultLayer(layer) {}

    // 添加行的链式调用
    UILayout& AddRow(UIRow row) {
        rows.push_back(std::move(row));
        return *this;
    }
    UILayout& AddRow(std::vector<UIComponent> comps, int gapX, int mt = 0, int mb = 0) {
        rows.push_back(UIRow(std::move(comps), gapX, mt, mb));
        return *this;
    }
    UILayout& AddComponent(UIComponent c) {
        rows.push_back(UIRow::Single(std::move(c)));
        return *this;
    }

    // 添加一行多个组件
    UILayout& AddRow(std::initializer_list<UIComponent> comps) {
        UIRow row;
        for (auto& c : comps) row.components.push_back(c);
        rows.push_back(std::move(row));
        return *this;
    }
};

// ══════════════════════════════════════════════════════════════════
// 6. 布局计算结果 — 单个组件在计算后的位置信息
// ══════════════════════════════════════════════════════════════════

struct UIComponentPlacement {
    HWND hwnd = nullptr;                 // 创建的控件句柄
    UIComponentType type;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    int layer = 0;                       // 最终层数
    int id = -1;
};

struct UILayoutResult {
    std::vector<UIComponentPlacement> placements;
    int totalHeight = 0;                 // 布局总高度
    int totalWidth = 0;                  // 布局总宽度

    // 根据控件 ID 查找 HWND (用于提取成员变量引用)
    HWND HwndForId(int id) const {
        for (const auto& p : placements) {
            if (p.id == id && p.hwnd) return p.hwnd;
        }
        return nullptr;
    }

    // 收集指定 ID 列表的所有 HWND
    std::vector<HWND> CollectHwnds() const {
        std::vector<HWND> hwnds;
        for (const auto& p : placements) {
            if (p.hwnd) hwnds.push_back(p.hwnd);
        }
        return hwnds;
    }
};

// ══════════════════════════════════════════════════════════════════
// 7. 预设样式工厂 — 统一的样式获取入口
// ══════════════════════════════════════════════════════════════════

namespace StylePreset {

// 编辑器参数面板默认标签样式
UIStyle EditorLabel();
// 普通标签样式
UIStyle NormalLabel();
// 编辑框样式
UIStyle EditField();
// 按钮样式
UIStyle NormalButton();
// 自绘绿色按钮样式
UIStyle ActionGreenButton();
// 自绘灰色按钮样式
UIStyle ActionGrayButton();
// 提示文本样式
UIStyle HintText();
// 复选框样式
UIStyle NormalCheckBox();

}  // namespace StylePreset

// ══════════════════════════════════════════════════════════════════
// 8. 核心 API
// ══════════════════════════════════════════════════════════════════

// 根据布局定义计算所有组件的位置 (不创建 HWND)
UILayoutResult CalculateLayout(const UILayout& layout, bool scaled = true);

// 根据布局定义创建所有控件并返回布局结果
UILayoutResult BuildLayout(HWND parent, const UILayout& layout, bool scaled = false);

// 缩放相关辅助
int ScaleX(int baseX);
int ScaleY(int baseY);
int ScaleW(int baseW);
int ScaleH(int baseH);

// 创建单个组件 HWND
HWND CreateComponent(HWND parent, const UIComponent& comp, int x, int y);

// 批量显示/隐藏布局中的控件
void ShowLayoutGroup(const UILayoutResult& result, bool visible);
