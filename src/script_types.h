// ──────────────────────────────────────────────────────────────────
// script_types.h — 核心数据模型
// 定义脚本动作类型枚举 (ActionType)、鼠标按键枚举 (MouseButtonType)，
// 以及包含所有动作参数的结构体 ScriptAction。
// 还提供容器判断辅助函数 (IsExpandableContainer 等)，被整个项目所依赖。
// ──────────────────────────────────────────────────────────────────
#pragma once

#include <windows.h>

#include <string>

// ── 脚本动作类型枚举 ──────────────────────────────────────────────
// 共 28 种动作类型，涵盖鼠标/键盘操作、流程控制、图像识别等
enum class ActionType {
    MoveMouse,
    Wait,
    MouseDown,
    MouseUp,
    MouseClick,
    KeyDown,
    KeyUp,
    KeyClick,
    Loop,
    EndLoop,
    DefineBlock,
    RunBlock,
    MousePlayback,
    RunMacro,
    HotkeyShortcut,
    QuickInput,
    ScrollWheel,
    FindImage,
    TextRecognition,
    If,
    Else,
    LockScreenshot,
    UnlockScreenshot,
    StopMacro,
    Goto,
    RunProgram,
    CloseProgram,
    OpenWebpage,
    OpenFile,
    TimerRecordTime,
    GetCursorPos,      // 获取当前光标位置
    CustomText,
    AiTextAnalysis,    // AI文字分析
    AiImageAnalysis,   // AI图片分析
    AiActionExecute    // AI动作执行
};

enum class MouseButtonType { Left, Right, Middle, X1, X2 };

// ── 脚本动作数据结构 ──────────────────────────────────────────────
// 每个字段对应某种动作类型的参数，未使用的字段保持默认值
struct ScriptAction {
    ActionType type = ActionType::MoveMouse;  // 动作类型
    std::wstring remark;                       // 用户备注文本
    std::wstring customText;                   // 自定义显示文本 (覆盖默认名称)
    int originalNo = 0;                       // 原始序号，序列化时保持
    int indent = 0;                           // 缩进层级 (0=顶层, >=1=子节点)
    int x = 0;                                // 目标 X 坐标 (MoveMouse 等)
    int y = 0;                                // 目标 Y 坐标
    int randomX = 0;                          // X 坐标随机偏移范围
    int randomY = 0;                          // Y 坐标随机偏移范围
    bool moveFromVar = false;                  // 是否从变量表达式解析移动坐标
    std::wstring moveVarExprX;                 // X 坐标变量表达式
    std::wstring moveVarExprY;                 // Y 坐标变量表达式
    MouseButtonType button = MouseButtonType::Left;  // 鼠标按键类型
    int clickCount = 1;                       // 点击/循环次数
    UINT keyVk = '7';                         // 虚拟键码
    std::wstring keyText = L"7";               // 按键显示名
    // ── 修饰键按下状态 (左右分别控制) ──
    bool holdLeftWin = false;
    bool holdRightWin = false;
    bool holdLeftCtrl = false;
    bool holdRightCtrl = false;
    bool holdLeftAlt = false;
    bool holdRightAlt = false;
    bool holdLeftShift = false;
    bool holdRightShift = false;
    double duration = 0.1;                    // 基本等待/持续时长 (秒)
    double randomDuration = 0.0;              // 最大随机时长 (秒)
    int loopCount = -1;                       // 循环次数 (-1=无限循环)
    std::wstring loopVarName;                  // 循环变量名
    bool loopFromVar = false;                  // 是否从变量表达式解析循环次数
    std::wstring loopVarExpr;                  // 循环次数变量表达式
    std::wstring blockName;                    // 宏指令块名称 (DefineBlock/RunBlock/RunMacro)
    std::wstring targetPath;                   // 目标脚本路径 (RunMacro/MousePlayback)
    int shortcutPreset = 0;                   // 快捷按键预设索引
    std::wstring inputText;                    // 快捷输入文本内容
    double charInterval = 0.01;              // 字符输入间隔 (秒)
    bool scrollVertical = true;               // 垂直滚动
    bool scrollHorizontal = false;            // 水平滚动
    int scrollSteps = 1;                     // 滚动步数
    int scrollDirection = 0;                  // 0=向上/左, 1=向下/右
    // ── 找图相关 ──
    int searchX1 = 0;                        // 搜索区域左上 X
    int searchY1 = 0;                        // 搜索区域左上 Y
    int searchX2 = 0;                        // 搜索区域右下 X
    int searchY2 = 0;                        // 搜索区域右下 Y
    bool searchFullScreen = true;             // 是否搜索整个屏幕
    std::wstring imagePath;                   // 图片路径
    double matchThreshold = 65.0;            // 匹配阈值 (百分比)
    double imageScale = 1.0;                // 缩放比例
    double imageScaleMin = 1.0;             // 最小缩放
    double imageScaleMax = 1.0;             // 最大缩放
    int findImageFollowUp = 0;              // 0=点击, 1=移动, 2=保存变量
    int offsetX = 0;                         // 点击偏移 X
    int offsetY = 0;                         // 点击偏移 Y
    bool findUntilFound = false;             // OCR 文字查找：是否循环直到找到
    std::wstring findTimeExpr = L"0";        // 找图时限（秒，可小数；0=只找一次；-1=直到找到；支持变量名）
    std::wstring matchVarName = L"matchRet";  // 匹配结果变量名
    // ── 文字识别相关 ──
    bool ocrRegionByImage = false;           // 获取文字：根据找图结果选取相对 OCR 区域
    bool ocrDigitsOnly = false;              // 纯数字识别（PaddleOCR 数字模式，失败时回退通用识别）
    int ocrResultMode = 0;                   // 0=获取文字, 1=文字查找
    std::wstring ocrSearchText;              // 文字查找目标（可含变量）
    int ocrFollowUp = 0;                     // 0=点击, 1=鼠标移动到, 2=保存到变量
    std::wstring conditionExpr;               // 条件表达式 (If 动作)
    std::wstring gotoStepExpr;                // 跳转目标序号（支持变量表达式）
    bool matchFileNameOnly = false;           // 关闭程序时仅匹配文件名
    // ── AI 动作通用 ──
    std::wstring aiPrompt;              // 用户 prompt（支持 {变量}）
    std::wstring aiOutputVarName;       // 输出变量名
    int aiOutputType = 0;              // 0=文本, 1=整数
    std::wstring aiModelName;           // 选用的模型名（对应 savedModels）
    int aiContextMode = 0;             // 0=无上下文, 1=宏上下文, 2=循环上下文, 3=指令块上下文
    int aiTimeoutSec = 30;            // API超时秒数
    // ── AI图片分析专用 ──
    double aiImageScale = 0.5;         // 截屏缩放比例（0.1-1.0），大图还会自动限边
    bool aiRegionByImage = false;       // 根据找图锚点确定 AI 分析区域
    std::wstring aiTargetImagePath;     // 锚定找图用的图片（aiRegionByImage=1 时）
    int aiSearchRegion = 0;           // 搜索区域：0=全屏,1=约左上,2=约右上,3=约左下,4=约右下,5=约中央,6=自定义
    int aiSearchX1 = 0;               // 自定义搜索区域 X1
    int aiSearchY1 = 0;               // 自定义搜索区域 Y1
    int aiSearchX2 = 0;               // 自定义搜索区域 X2
    int aiSearchY2 = 0;               // 自定义搜索区域 Y2
    // ── AI动作执行专用 ──
    bool aiWithImage = false;          // 是否附带截图调用 API
    int aiMaxSteps = 10;               // 最大执行步骤数（-1=不限制）
    std::wstring aiFallbackValue;        // API失败降级值
    bool aiConfirmExecute = false;       // AI动作执行：确认后执行
};

// ── 容器动作判断辅助函数 ────────────────────────────────────────
// 判断动作类型是否包含子动作 (可展开/折叠)
inline bool IsExpandableContainer(ActionType type) {
    return type == ActionType::Loop || type == ActionType::DefineBlock
        || type == ActionType::If || type == ActionType::Else;
}

// 判断是否作为子树容器 (用于拖拽嵌套逻辑)
inline bool IsSubtreeContainer(ActionType type) {
    return type == ActionType::Loop || type == ActionType::DefineBlock
        || type == ActionType::If || type == ActionType::Else;
}

// 判断动作是否在执行主流程中跳过 (定义块不在主流程中执行)
inline bool SkipsInMainFlow(ActionType type) {
    return type == ActionType::DefineBlock;
}
