#pragma once

#include <windows.h>

#include <string>

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
    CustomText
};

enum class MouseButtonType { Left, Right, Middle };

struct ScriptAction {
    ActionType type = ActionType::MoveMouse;
    std::wstring remark;
    std::wstring customText;
    int originalNo = 0;
    int indent = 0;
    int x = 0;
    int y = 0;
    int randomX = 0;
    int randomY = 0;
    MouseButtonType button = MouseButtonType::Left;
    int clickCount = 1;
    UINT keyVk = '7';
    std::wstring keyText = L"7";
    bool holdLeftWin = false;
    bool holdRightWin = false;
    bool holdLeftCtrl = false;
    bool holdRightCtrl = false;
    bool holdLeftAlt = false;
    bool holdRightAlt = false;
    bool holdLeftShift = false;
    bool holdRightShift = false;
    double duration = 0.1;
    double randomDuration = 0.0;
    int loopCount = -1;
    std::wstring loopVarName;
    bool loopFromVar = false;
    std::wstring loopVarExpr;
    std::wstring blockName;
    std::wstring targetPath;
    int shortcutPreset = 0;
    std::wstring inputText;
    double charInterval = 0.01;
    bool scrollVertical = true;
    bool scrollHorizontal = false;
    int scrollSteps = 1;
    int scrollDirection = 0;
    int searchX1 = 0;
    int searchY1 = 0;
    int searchX2 = 0;
    int searchY2 = 0;
    bool searchFullScreen = true;
    std::wstring imagePath;
    double matchThreshold = 65.0;
    double imageScale = 1.0;
    double imageScaleMin = 1.0;
    double imageScaleMax = 1.0;
    int findImageFollowUp = 0;
    int offsetX = 0;
    int offsetY = 0;
    bool findUntilFound = false;
    std::wstring matchVarName = L"matchRet";
};

inline bool IsExpandableContainer(ActionType type) {
    return type == ActionType::Loop || type == ActionType::DefineBlock;
}

inline bool IsSubtreeContainer(ActionType type) {
    return type == ActionType::Loop || type == ActionType::DefineBlock;
}

inline bool SkipsInMainFlow(ActionType type) {
    return type == ActionType::DefineBlock;
}
