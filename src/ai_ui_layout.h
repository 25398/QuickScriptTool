#pragma once
//
// ai_ui_layout.h — UI 布局记忆 + 相对网格推断（通用，不限游戏）
//
// 解决的问题：**同一个界面元素不该被反复识图定位**。
//   · 草坪格子 / 卡槽 / 桌面图标阵列 / 工具栏按钮 —— 位置在一次会话里基本不变；
//   · 定位成功一次后记住坐标，后面直接用（0 次识图）；
//   · 更进一步：定位到**一个**元素后，用画面自身的重复周期推断出**整片**网格坐标
//     （一行卡槽、5×9 草坪、图标阵列），之后按 (行,列) 直接点。
//
// 这不是我们拍脑袋想出来的，业界三种成熟做法我们取前两种：
//   ① **元素定位缓存 / UI 对象仓库**：Midscene 的 caching（缓存 AI 规划与**定位**结果，
//      https://v0.midscenejs.com/zh/caching ）、UiPath/Power Automate 的 UI Object Repository
//      （把元素描述与坐标存成可复用仓库，https://patents.justia.com/patent/11809846 ）。
//   ② **相对定位**：Sikuli 的 `Region.right()/below()/nearby()` —— 由一个锚点推导邻居，
//      而不是对每个目标重新匹配（https://lists.launchpad.net/sikuli-driver/msg55523.html ）。
//   ③ 一次解析整屏、模型只引用 ID（OmniParser / Set-of-Mark，微软开源，
//      https://cloud.tencent.com.cn/developer/article/2502979 ）—— 我们已有等价物（UIA 树 /
//      DOM 树 / locateAndClick(targets=[…])），本模块补的是 ① 和 ②。
//
// 失效原则（宁可重新识图，也不能拿旧坐标乱点）：
//   · 键里含**窗口身份**（标题|类名|进程|客户区尺寸）与**屏幕尺寸** —— 换窗口/挪窗口/改分辨率
//     一律视为新界面，旧坐标直接作废；
//   · 点下去画面没变化 → 该锚点立刻作废（沿用定位缓存那套「错点不固化」的规矩）。

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

/// 归一化后的目标描述 + 窗口身份 + 屏幕尺寸 = 布局记忆的键
struct AiUiLayoutKey {
    std::wstring target;
    std::wstring windowIdentity;
    int screenW = 0;
    int screenH = 0;

    bool empty() const { return target.empty() || windowIdentity.empty(); }
    bool operator==(const AiUiLayoutKey& o) const {
        return target == o.target && windowIdentity == o.windowIdentity
            && screenW == o.screenW && screenH == o.screenH;
    }
};

struct AiUiLayoutRect {
    int x1 = 0;
    int y1 = 0;
    int x2 = 0;
    int y2 = 0;
    bool valid() const { return x2 > x1 && y2 > y1; }
    int cx() const { return (x1 + x2) / 2; }
    int cy() const { return (y1 + y2) / 2; }
    int width() const { return x2 - x1; }
    int height() const { return y2 - y1; }
};

/// 规则网格：定位到其中一个元素后，用它 + 画面周期推出整片坐标。
/// (row,col) 相对**锚点格**：(0,0)=锚点本身，(0,1)=右边一格，(1,0)=下面一行。
struct AiUiGridSpec {
    int originX = 0;   // 锚点格中心（屏幕坐标）
    int originY = 0;
    int stepX = 0;     // 列间距（>0 向右）
    int stepY = 0;     // 行间距（>0 向下）
    int cols = 0;      // 锚点右侧还有几列（含锚点列则为 cols+1）
    int rows = 0;
    bool valid() const {
        return (stepX > 0 || stepY > 0) && cols >= 0 && rows >= 0;
    }
};

/// 取相对锚点的 (row,col) 格中心；越界返回 false。
bool AiUiGridCellCenter(const AiUiGridSpec& grid, int row, int col, int& x, int& y);

/// 从**一帧灰度图**（8bit，单通道，stride 可含行padding）与已定位到的锚点框，
/// 用「自位移平均绝对差」找画面在该方向上的重复周期（UI 网格的经典做法）：
/// 对每个候选 lag 算 anchor 带内 |I(x) − I(x+lag)| 的均值，取显著极小值。
/// 返回 false = 没有明显周期性（不规则界面）→ 调用方老实逐次识图，别硬套网格。
bool AiUiDetectGridPeriod(const uint8_t* gray, int width, int height, int stride,
    const AiUiLayoutRect& anchor, int minPeriod, int maxPeriod,
    int& periodX, int& periodY);

/// HBITMAP → 8bit 灰度缓冲（自己按 GDI 取位，不依赖 OpenCV，便于自检）。
bool AiUiBitmapToGrayBytes(HBITMAP bmp, std::vector<uint8_t>& out, int& width, int& height,
    int& stride);

/// 记忆命中前的**硬校验**：记一次坐标同时留一份「外观签名」（格子平均亮度），
/// 命中时先用当前画面重算签名比对 —— 通过了才敢 0 识图直达。
/// 这是「点错按钮」这类最危险故障的闸：界面只要变过（面板关了/换页/换窗口内容），
/// 签名就对不上 → 自动回退真识图。kSigN=8 → 64 字节，毫秒级。
inline constexpr int kAiUiSigN = 8;

/// 从一帧灰度图按 rect（自动外扩 20% 边距）算签名；失败返回 false
bool AiUiLayoutSignature(const uint8_t* gray, int width, int height, int stride,
    const AiUiLayoutRect& rect, uint8_t outSig[kAiUiSigN * kAiUiSigN]);

/// 记住/比对签名（同键）。比对容差 tol：平均绝对差 ≤ tol 判为「还是它」
void AiUiLayoutRememberSignature(const AiUiLayoutKey& key,
    const uint8_t sig[kAiUiSigN * kAiUiSigN]);
bool AiUiLayoutSignatureMatches(const AiUiLayoutKey& key,
    const uint8_t sig[kAiUiSigN * kAiUiSigN], int tol = 20);

/// 记住一个已定位成功的元素（屏幕坐标框）。多次记住会**取平均**让坐标越用越准；
/// 偏差 >24px 视为界面确实动过 → 换新值。
void AiUiLayoutRemember(const AiUiLayoutKey& key, const AiUiLayoutRect& rect);
/// 取回；返回 false=没有、或连续 2 次「照记忆点没反应」被判过期（条目保留，下次识图会刷新）
bool AiUiLayoutRecall(const AiUiLayoutKey& key, AiUiLayoutRect& out);
/// 记一笔「照记忆坐标点下去没变化」。**单次不删条目**（小范围颜色采样会误报，
/// 工具本身耗时几秒后画面早变了）；连续 2 次才让 Recall 回退真识图，并作废网格重新推断。
/// 用户的意见：算出来的坐标不总是准，界面没大变化就该留着并继续积累数据。
void AiUiLayoutNoteClickNoEffect(const AiUiLayoutKey& key);
/// 当前处于「过期待刷新」状态的条目数（诊断用）
int AiUiLayoutStaleCount();
/// 记住/取回该锚点的网格（同一键）
void AiUiLayoutRememberGrid(const AiUiLayoutKey& key, const AiUiGridSpec& grid);
bool AiUiLayoutRecallGrid(const AiUiLayoutKey& key, AiUiGridSpec& out);
/// 点下去画面没变化 → 这条记忆是错的，作废（含网格）
void AiUiLayoutForget(const AiUiLayoutKey& key);
/// 每次开始跑脚本清空（绝不让上一次运行的坐标影响这一次）
void AiUiLayoutClear();
int AiUiLayoutCount();
int AiUiLayoutHitCount();
int AiUiLayoutMissCount();

/// 窗口身份串：标题|类名|进程名|客户区尺寸。任一项变化即视为新界面。
/// hwnd 为空返回空串（调用方据此跳过布局记忆）。
std::wstring AiUiWindowIdentityForLayout(HWND hwnd);
