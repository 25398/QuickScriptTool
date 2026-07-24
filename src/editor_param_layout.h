// ──────────────────────────────────────────────────────────────────
// editor_param_layout.h — 编辑器参数面板布局定义
//
// 面板 base-left=kParamPanelLeftDesign，字段宽度=kParamFieldWidth（含左右边线余量）。
// 1200×1080 下对应 x≈923，宽约 228px（滚动条列外）。
// ──────────────────────────────────────────────────────────────────
#pragma once

#include "ui_component.h"
#include "config.h"

namespace EditorParamLayout {

constexpr int kPanelLeft = kFindContentLeft;   // 785，找图等同宽区块仍用此值
constexpr int kParamPanelLeft = kParamPanelLeftDesign;
constexpr int kPanelWidth = kParamFieldWidth;
constexpr int kPanelTextFieldH = 64;  // 多行/长文本输入框（如提示词、URL）
constexpr int kPanelSingleFieldH = 22; // 单行短内容输入框（与超时等字段一致）

// ── 控件 ID ──
constexpr int EID_MoveX                   = 1015;
constexpr int EID_MoveY                   = 1016;
constexpr int EID_MoveRandomX             = 1017;
constexpr int EID_MoveRandomY             = 1018;
constexpr int EID_MoveFromVar             = 1019;
constexpr int EID_MoveVarX                = 1020;
constexpr int EID_MoveVarY                = 1021;
constexpr int EID_MoveRelX                = 1196;
constexpr int EID_MoveRelY                = 1197;
constexpr int EID_MoveRelRandomX          = 1198;
constexpr int EID_MoveRelRandomY          = 1199;
constexpr int EID_ClickButton             = 1022;
constexpr int EID_ClickCount              = 1023;
constexpr int EID_ClickWait               = 1024;
constexpr int EID_ClickRandom             = 1025;
constexpr int EID_WaitDuration            = 1026;
constexpr int EID_WaitRandom              = 1027;
constexpr int EID_KeyCapture              = 1031;
constexpr int EID_ClickLWin               = 1032;
constexpr int EID_ClickRWin               = 1033;
constexpr int EID_ClickLCtrl              = 1034;
constexpr int EID_ClickRCtrl              = 1035;
constexpr int EID_ClickLAlt               = 1036;
constexpr int EID_ClickRAlt               = 1037;
constexpr int EID_ClickLShift             = 1038;
constexpr int EID_ClickRShift             = 1039;
constexpr int EID_KeyLWin                 = 1040;
constexpr int EID_KeyRWin                 = 1041;
constexpr int EID_KeyLCtrl                = 1042;
constexpr int EID_KeyRCtrl                = 1043;
constexpr int EID_KeyLAlt                 = 1044;
constexpr int EID_KeyRAlt                 = 1045;
constexpr int EID_KeyLShift               = 1046;
constexpr int EID_KeyRShift               = 1047;
constexpr int EID_Crosshair               = 1048;
constexpr int EID_LoopCount               = 1049;
constexpr int EID_LoopFromVar             = 1050;
constexpr int EID_LoopVarExpr             = 1051;
constexpr int EID_LoopVarName             = 1052;
constexpr int EID_DefineBlockName         = 1053;
constexpr int EID_RunBlockCombo           = 1054;
constexpr int EID_KeyPressCapture         = 1055;
constexpr int EID_MousePressButton        = 1056;
constexpr int EID_MousePressLWin          = 1057;
constexpr int EID_MousePressRWin          = 1058;
constexpr int EID_MousePressLCtrl         = 1059;
constexpr int EID_MousePressRCtrl         = 1060;
constexpr int EID_MousePressLAlt          = 1061;
constexpr int EID_MousePressRAlt          = 1062;
constexpr int EID_MousePressLShift        = 1063;
constexpr int EID_MousePressRShift        = 1064;
constexpr int EID_KeyPressLWin            = 1065;
constexpr int EID_KeyPressRWin            = 1066;
constexpr int EID_KeyPressLCtrl           = 1067;
constexpr int EID_KeyPressRCtrl           = 1068;
constexpr int EID_KeyPressLAlt            = 1069;
constexpr int EID_KeyPressRAlt            = 1070;
constexpr int EID_KeyPressLShift          = 1071;
constexpr int EID_KeyPressRShift          = 1072;
constexpr int EID_HotkeyShortcutCombo     = 1073;
constexpr int EID_HotkeyShortcutCount     = 1074;
constexpr int EID_HotkeyShortcutWait      = 1075;
constexpr int EID_HotkeyShortcutRandom    = 1076;
constexpr int EID_QuickInputText          = 1077;
constexpr int EID_QuickInputVarCombo      = 1078;
constexpr int EID_QuickInputInsert        = 1079;
constexpr int EID_QuickInputCharInterval  = 1080;
constexpr int EID_QuickInputCount         = 1081;
constexpr int EID_QuickInputWait          = 1082;
constexpr int EID_QuickInputRandom        = 1083;
constexpr int EID_RunMacroCombo           = 1084;
constexpr int EID_MousePlaybackCombo      = 1085;
constexpr int EID_MousePlaybackCount      = 1086;
constexpr int EID_MousePlaybackWait       = 1087;
constexpr int EID_MousePlaybackRandom     = 1088;
constexpr int EID_ScrollVertical          = 1089;
constexpr int EID_ScrollHorizontal        = 1090;
constexpr int EID_ScrollSteps             = 1091;
constexpr int EID_ScrollDirection         = 1092;
constexpr int EID_ScrollCount             = 1093;
constexpr int EID_ScrollWait              = 1094;
constexpr int EID_ScrollRandom            = 1095;
constexpr int EID_FindFullScreen          = 1096;
constexpr int EID_FindSelectRegion        = 1097;
constexpr int EID_FindX1                  = 1098;
constexpr int EID_FindY1                  = 1099;
constexpr int EID_FindX2                  = 1100;
constexpr int EID_FindY2                  = 1101;
constexpr int EID_FindTest                = 1102;
constexpr int EID_FindScreenshot          = 1103;
constexpr int EID_FindLocalImage          = 1104;
constexpr int EID_FindClearImage          = 1105;
constexpr int EID_FindImagePreview        = 1106;
constexpr int EID_FindMatchThreshold      = 1107;
constexpr int EID_FindScaleMin            = 1108;
constexpr int EID_FindScaleMax            = 1109;
constexpr int EID_FindFollowUp            = 1110;
constexpr int EID_FindOffsetX             = 1111;
constexpr int EID_FindOffsetY             = 1112;
constexpr int EID_FindSelectOffset        = 1113;
constexpr int EID_FindTime                = 1114;
constexpr int EID_FindMatchVar            = 1115;
constexpr int EID_OcrFullScreen           = 1116;
constexpr int EID_OcrSelectRegion         = 1117;
constexpr int EID_OcrX1                   = 1118;
constexpr int EID_OcrY1                   = 1119;
constexpr int EID_OcrX2                   = 1120;
constexpr int EID_OcrY2                   = 1121;
constexpr int EID_OcrResultMode           = 1122;
constexpr int EID_OcrSearchText           = 1123;
constexpr int EID_OcrSearchVarCombo       = 1124;
constexpr int EID_OcrSearchVarInsert      = 1125;
constexpr int EID_OcrFollowUp             = 1126;
constexpr int EID_OcrOffsetX              = 1127;
constexpr int EID_OcrOffsetY              = 1128;
constexpr int EID_OcrSelectOffset         = 1129;
constexpr int EID_OcrUntilFound           = 1130;
constexpr int EID_OcrResultVar            = 1131;
constexpr int EID_OcrTest                 = 1132;
constexpr int EID_OcrInstallDep           = 1133;
constexpr int EID_OcrRegionByImage        = 1134;
constexpr int EID_OcrFindSelectRegion     = 1135;
constexpr int EID_OcrFindScreenshot       = 1136;
constexpr int EID_OcrFindLocalImage       = 1137;
constexpr int EID_OcrFindClearImage       = 1138;
constexpr int EID_OcrFindImagePreview     = 1139;
constexpr int EID_OcrFindMatchThreshold   = 1140;
constexpr int EID_OcrFindScaleMin         = 1141;
constexpr int EID_OcrFindScaleMax         = 1142;
constexpr int EID_OcrDigitsOnly           = 1143;
constexpr int EID_IfVarCombo              = 1144;
constexpr int EID_IfOperator              = 1145;
constexpr int EID_IfValue                 = 1146;
constexpr int EID_IfConnector             = 1147;
constexpr int EID_IfAddCondition          = 1148;
constexpr int EID_IfConditionList         = 1149;
constexpr int EID_RunProgramCombo         = 1150;
constexpr int EID_RunProgramPath          = 1151;
constexpr int EID_RunProgramBrowse        = 1152;
constexpr int EID_RunProgramCrosshair     = 1153;
constexpr int EID_RunProgramArgs          = 1154;
constexpr int EID_CloseProgramPath        = 1155;
constexpr int EID_CloseProgramBrowse      = 1156;
constexpr int EID_CloseProgramCrosshair   = 1157;
constexpr int EID_CloseProgramMatchFileName=1158;
constexpr int EID_OpenWebpageUrl          = 1159;
constexpr int EID_OpenFilePath            = 1160;
constexpr int EID_OpenFileBrowse          = 1161;
constexpr int EID_TimerVarName            = 1162;
constexpr int EID_LoopTypeCombo           = 0;
constexpr int EID_AiPrompt                = 1163;
constexpr int EID_AiInsertVar             = 1164;
constexpr int EID_AiVarCombo              = 1165;
constexpr int EID_AiModel                 = 1166;
constexpr int EID_AiContextMode           = 1167;
constexpr int EID_AiOutputVar             = 1168;
constexpr int EID_AiOutputType            = 1169;
constexpr int EID_AiTimeout               = 1170;
constexpr int EID_AiFallback              = 1171;
constexpr int EID_AiImageScale            = 1172;
constexpr int EID_AiRegionByImage         = 1173;
constexpr int EID_AiRegionByImage2        = 1174;
constexpr int EID_AiFindSelectRegion      = 1175;
constexpr int EID_AiFindMatchThreshold    = 1176;
constexpr int EID_AiFindScaleMin          = 1177;
constexpr int EID_AiFindScaleMax          = 1178;
constexpr int EID_AiTargetPreview         = 1179;
constexpr int EID_AiTargetScreenshot      = 1180;
constexpr int EID_AiTargetLocal           = 1181;
constexpr int EID_AiTargetClear           = 1182;
constexpr int EID_AiFullScreen            = 1183;
constexpr int EID_AiSelectRegion          = 1184;
constexpr int EID_AiSearchRegion          = 1185;
constexpr int EID_AiSearchX1              = 1186;
constexpr int EID_AiSearchY1              = 1187;
constexpr int EID_AiSearchX2              = 1188;
constexpr int EID_AiSearchY2              = 1189;
constexpr int EID_AiMaxSteps              = 1190;
constexpr int EID_AiWithImage             = 1191;
constexpr int EID_AiConfirm               = 1192;
constexpr int EID_AiMaxStepsHint          = 1193;
constexpr int EID_CursorPosVarName        = 1194;
constexpr int EID_GotoStepExpr            = 1195;

// 辅助: Indent 将第一个组件推到指定 x (相对于 panel left), Gap 为精确像素间距
inline UIComponent Indent(int x) { return UIComponent::Spacer(x - kParamPanelLeft, 0); }
inline UIComponent Gap(int px)   { return UIComponent::Spacer(px, 0); }

// ══════════════════════════════════════════════════════════════════
// 1. 移动鼠标到 (y 从 180 开始)
// ══════════════════════════════════════════════════════════════════
inline UILayout MoveMouse() {
    return UILayout(kParamPanelLeft, 180, kPanelWidth)
        .AddRow({
            UIComponent::Label(L"移动到(左上角为0,0)", -1, 190, 25),
        }, 0, 0, 9)                             // gap to next: 214 - (180+25) = 9
        .AddRow({
            UIComponent::Label(L"X:", -1, 25, 22),
            Gap(1),
            UIComponent::Edit(L"0", EID_MoveX, 87, 22),
            Gap(1),
            UIComponent::Label(L"±随机:", -1, 50, 22),
            Gap(5),
            UIComponent::Edit(L"0", EID_MoveRandomX, 25, 22),
        }, 0, 0, 15)                              // 251 - (214+22) = 15
        .AddRow({
            UIComponent::Label(L"Y:", -1, 25, 22),
            Gap(1),
            UIComponent::Edit(L"0", EID_MoveY, 87, 22),
            Gap(1),
            UIComponent::Label(L"±随机:", -1, 50, 22),
            Gap(5),
            UIComponent::Edit(L"0", EID_MoveRandomY, 25, 22),
        }, 0, 0, 10)                              // 283 - (251+22) = 10
        .AddRow({
            UIComponent::GreenButton(L"拖动准星获取坐标", EID_Crosshair, 186, 32),
        }, 0, 0, 7)                               // 322 - (283+32) = 7
        .AddRow({
            UIComponent::CheckBox(L"来自变量表达式", EID_MoveFromVar, 180, 25),
        }, 0, 0, 7)                               // 354 - (322+25) = 7
        .AddRow({
            UIComponent::Label(L"X:", -1, 25, 22),
            Gap(1),
            UIComponent::Edit(L"0", EID_MoveVarX, 168, 22),
        }, 0, 0, 15)                              // 391 - (354+22) = 15
        .AddRow({
            UIComponent::Label(L"Y:", -1, 25, 22),
            Gap(1),
            UIComponent::Edit(L"0", EID_MoveVarY, 168, 22),
        }, 0, 0, 8)                               // 421 - (391+22) = 8
        .AddRow({
            UIComponent::Hint(L"*提示:可使用来自找图、找色，获取颜色，文字识别保存到变量中的值", kParamFieldWidth, 56),
        });
}

// ══════════════════════════════════════════════════════════════════
// 1b. 相对移动鼠标 (dx/dy)
// ══════════════════════════════════════════════════════════════════
inline UILayout MoveMouseRelative() {
    return UILayout(kParamPanelLeft, 180, kPanelWidth)
        .AddRow({
            UIComponent::Label(L"相对位移(像素,可负)", -1, 190, 25),
        }, 0, 0, 9)
        .AddRow({
            UIComponent::Label(L"dx:", -1, 30, 22),
            Gap(1),
            UIComponent::Edit(L"0", EID_MoveRelX, 87, 22),
            Gap(1),
            UIComponent::Label(L"±随机:", -1, 50, 22),
            Gap(5),
            UIComponent::Edit(L"0", EID_MoveRelRandomX, 25, 22),
        }, 0, 0, 15)
        .AddRow({
            UIComponent::Label(L"dy:", -1, 30, 22),
            Gap(1),
            UIComponent::Edit(L"0", EID_MoveRelY, 87, 22),
            Gap(1),
            UIComponent::Label(L"±随机:", -1, 50, 22),
            Gap(5),
            UIComponent::Edit(L"0", EID_MoveRelRandomY, 25, 22),
        }, 0, 0, 10)
        .AddRow({
            UIComponent::Hint(L"*提示:Raw 逐包录制；回放关加速+绝对时间轴+高优先级。请重新录制后验证", kParamFieldWidth, 56),
        });
}

// ══════════════════════════════════════════════════════════════════
// 2. 等待 (y 从 185 开始)
// ══════════════════════════════════════════════════════════════════
inline UILayout Wait() {
    return UILayout(kParamPanelLeft, 185, kPanelWidth)
        .AddRow({
            UIComponent::Label(L"等待时间", -1, 90, 25),
        }, 0, 0, 6)                               // 216 - (185+25) = 6
        .AddRow({
            UIComponent::Edit(L"0.500", EID_WaitDuration, 76, 22),
            Gap(7),
            UIComponent::Label(L"秒", -1, 35, 22),
        }, 0, 0, 20)                              // 258 - (216+22) = 20
        .AddRow({
            UIComponent::Label(L"最大随机时间", -1, 120, 25),
        }, 0, 0, 6)                               // 289 - (258+25) = 6
        .AddRow({
            UIComponent::Edit(L"0.000", EID_WaitRandom, 76, 22),
            Gap(7),
            UIComponent::Label(L"秒", -1, 35, 22),
        }, 0, 0, 8)                               // 319 - (289+22) = 8
        .AddRow({
            UIComponent::Hint(L"*提示:等待总时间=等待时间+随机(0~最大随机时间)", kParamFieldWidth, 40),
        });
}

// ══════════════════════════════════════════════════════════════════
// 3. 鼠标点击
// ══════════════════════════════════════════════════════════════════
inline UILayout MouseClick() {
    return UILayout(kParamPanelLeft, 174, kPanelWidth)
        .AddRow({
            UIComponent::EditorLabel(L"选择鼠标键", -1, kPanelWidth, 22),
        }, 0, 0, 8)                                // combo at y=204 (174+22+8)
        .AddRow({
            UIComponent::Combo(L"左键", EID_ClickButton, kPanelWidth, 21),
        }, 0, 0, 11)                               // y=225, gap: 257-(204+21)=32 → too much, use explicit gap
        // The original "同时按住" label starts at y=257
        // We'll use a row with just a margin since combo ends at 204+21=225
        .AddRow({
            UIComponent::Label(L"同时按住", -1, 120, 25),
        }, 0, 0, 6)                               // y=288, gap: 288-(257+25)=6
        .AddRow({
            UIComponent::CheckBox(L"左Win", EID_ClickLWin, 80, 25),
            Gap(19),
            UIComponent::CheckBox(L"右Win", EID_ClickRWin, 80, 25),
        }, 0, 0, 6)                               // y=319
        .AddRow({
            UIComponent::CheckBox(L"左Ctrl", EID_ClickLCtrl, 80, 25),
            Gap(19),
            UIComponent::CheckBox(L"右Ctrl", EID_ClickRCtrl, 80, 25),
        }, 0, 0, 6)                               // y=350
        .AddRow({
            UIComponent::CheckBox(L"左Alt", EID_ClickLAlt, 80, 25),
            Gap(19),
            UIComponent::CheckBox(L"右Alt", EID_ClickRAlt, 80, 25),
        }, 0, 0, 6)                               // y=381
        .AddRow({
            UIComponent::CheckBox(L"左Shift", EID_ClickLShift, 86, 25),
            Gap(13),
            UIComponent::CheckBox(L"右Shift", EID_ClickRShift, 86, 25),
        }, 0, 0, 6)                               // y=412
        .AddRow({
            UIComponent::Label(L"循环次数", -1, 75, 22),
            Gap(16),
            UIComponent::Edit(L"0", EID_ClickCount, 54, 22),
        }, 0, 0, 15)                              // y=449, gap=449-(412+22)=15
        .AddRow({
            UIComponent::Label(L"重复间隔", -1, 75, 22),
            Gap(16),
            UIComponent::Edit(L"0.010", EID_ClickWait, 54, 22),
            Gap(6),
            UIComponent::Label(L"秒", -1, 32, 25),
        }, 0, 0, 13)                              // y=487, gap=487-(449+max(22,25))=13
        .AddRow({
            UIComponent::Label(L"随机间隔", -1, 75, 22),
            Gap(16),
            UIComponent::Edit(L"0.000", EID_ClickRandom, 54, 22),
            Gap(6),
            UIComponent::Label(L"秒", -1, 32, 25),
        });
}

// ══════════════════════════════════════════════════════════════════
// 4/5. 鼠标按下/松开 (共用，无循环次数等)
// ══════════════════════════════════════════════════════════════════
inline UILayout MousePress() {
    return UILayout(kParamPanelLeft, 174, kPanelWidth)
        .AddRow({
            UIComponent::EditorLabel(L"选择鼠标键", -1, kPanelWidth, 22),
        }, 0, 0, 8)
        .AddRow({
            UIComponent::Combo(L"左键", EID_MousePressButton, kPanelWidth, 21),
        }, 0, 0, 11)
        .AddRow({
            UIComponent::Label(L"同时按住", -1, 120, 25),
        }, 0, 0, 6)
        .AddRow({
            UIComponent::CheckBox(L"左Win", EID_MousePressLWin, 80, 25),
            Gap(19),
            UIComponent::CheckBox(L"右Win", EID_MousePressRWin, 80, 25),
        }, 0, 0, 6)
        .AddRow({
            UIComponent::CheckBox(L"左Ctrl", EID_MousePressLCtrl, 80, 25),
            Gap(19),
            UIComponent::CheckBox(L"右Ctrl", EID_MousePressRCtrl, 80, 25),
        }, 0, 0, 6)
        .AddRow({
            UIComponent::CheckBox(L"左Alt", EID_MousePressLAlt, 80, 25),
            Gap(19),
            UIComponent::CheckBox(L"右Alt", EID_MousePressRAlt, 80, 25),
        }, 0, 0, 6)
        .AddRow({
            UIComponent::CheckBox(L"左Shift", EID_MousePressLShift, 86, 25),
            Gap(13),
            UIComponent::CheckBox(L"右Shift", EID_MousePressRShift, 86, 25),
        });
}

// ══════════════════════════════════════════════════════════════════
// 6. 鼠标回放
// ══════════════════════════════════════════════════════════════════
inline UILayout MousePlayback() {
    return UILayout(kParamPanelLeft, 174, kPanelWidth)
        .AddRow({
            UIComponent::EditorLabel(L"请选择用于回放的鼠标录制", -1, kPanelWidth, 22),
        }, 0, 0, 8)
        .AddRow({
            UIComponent::Combo(L"", EID_MousePlaybackCombo, kPanelWidth, 21),
        }, 0, 0, 19)                               // combo ends at 225, label at 244, gap=19
        .AddRow({
            UIComponent::Label(L"循环次数", -1, 75, 22),
            Gap(16),
            UIComponent::Edit(L"0", EID_MousePlaybackCount, 54, 22),
        }, 0, 0, 15)
        .AddRow({
            UIComponent::Label(L"重复间隔", -1, 75, 22),
            Gap(16),
            UIComponent::Edit(L"0.010", EID_MousePlaybackWait, 54, 22),
            Gap(6),
            UIComponent::Label(L"秒", -1, 32, 25),
        }, 0, 0, 12)
        .AddRow({
            UIComponent::Label(L"随机间隔", -1, 75, 22),
            Gap(16),
            UIComponent::Edit(L"0.000", EID_MousePlaybackRandom, 54, 22),
            Gap(6),
            UIComponent::Label(L"秒", -1, 32, 25),
        });
}

// ══════════════════════════════════════════════════════════════════
// 7. 运行鼠标宏
// ══════════════════════════════════════════════════════════════════
inline UILayout RunMacro() {
    return UILayout(kParamPanelLeft, 174, kPanelWidth)
        .AddRow({
            UIComponent::EditorLabel(L"请选择用于运行的鼠标宏", -1, kPanelWidth, 22),
        }, 0, 0, 8)
        .AddRow({
            UIComponent::Combo(L"", EID_RunMacroCombo, kPanelWidth, 21),
        });
}

// ══════════════════════════════════════════════════════════════════
// 8. 滚动滚轮
// ══════════════════════════════════════════════════════════════════
inline UILayout ScrollWheel() {
    return UILayout(kParamPanelLeft, 180, kPanelWidth)
        .AddRow({
            UIComponent::Label(L"滚动类型", -1, 90, 25),
        }, 0, 0, 5)                                // y=210
        .AddRow({
            UIComponent::CheckBox(L"垂直", EID_ScrollVertical, 70, 25),
            Gap(29),
            UIComponent::CheckBox(L"水平", EID_ScrollHorizontal, 70, 25),
        }, 0, 0, 9)                                // y=244
        .AddRow({
            UIComponent::Label(L"滚动步数", -1, 75, 22),
            Gap(26),
            UIComponent::Edit(L"1", EID_ScrollSteps, 54, 22),
        }, 0, 0, 8)                                // y=270  label then combo
        .AddRow({                                  // EditorLabel at 790 (left=0 relative)
            UIComponent::EditorLabel(L"滚动方向", -1, kPanelWidth, 22),
        }, 0, 0, 10)                               // y=302
        .AddRow({
            UIComponent::Combo(L"向上/左", EID_ScrollDirection, kPanelWidth, 21),
        }, 0, 0, 11)                               // 循环/等待区紧跟滚动方向
        .AddRow({
            UIComponent::Label(L"循环次数", -1, 75, 22),
            Gap(16),
            UIComponent::Edit(L"0", EID_ScrollCount, 54, 22),
        }, 0, 0, 15)                               // y=449
        .AddRow({
            UIComponent::Label(L"重复间隔", -1, 75, 22),
            Gap(16),
            UIComponent::Edit(L"0.010", EID_ScrollWait, 54, 22),
            Gap(6),
            UIComponent::Label(L"秒", -1, 32, 25),
        }, 0, 0, 13)                               // y=487
        .AddRow({
            UIComponent::Label(L"随机间隔", -1, 75, 22),
            Gap(16),
            UIComponent::Edit(L"0.000", EID_ScrollRandom, 54, 22),
            Gap(6),
            UIComponent::Label(L"秒", -1, 32, 25),
        });
}

// ══════════════════════════════════════════════════════════════════
// 9. 按键点击
// ══════════════════════════════════════════════════════════════════
inline UILayout KeyClick() {
    return UILayout(kParamPanelLeft, 174, kPanelWidth)
        .AddRow({
            UIComponent::EditorLabel(L"选择键盘按键", -1, kPanelWidth, 22),
        }, 0, 0, 8)
        .AddRow({
            UIComponent::CaptureField(L"点击修改", EID_KeyCapture, kPanelWidth, 21),
        }, 0, 0, 11)
        .AddRow({
            UIComponent::Label(L"同时按住", -1, 120, 25),
        }, 0, 0, 6)
        .AddRow({
            UIComponent::CheckBox(L"左Win", EID_KeyLWin, 80, 25),
            Gap(19),
            UIComponent::CheckBox(L"右Win", EID_KeyRWin, 80, 25),
        }, 0, 0, 6)
        .AddRow({
            UIComponent::CheckBox(L"左Ctrl", EID_KeyLCtrl, 80, 25),
            Gap(19),
            UIComponent::CheckBox(L"右Ctrl", EID_KeyRCtrl, 80, 25),
        }, 0, 0, 6)
        .AddRow({
            UIComponent::CheckBox(L"左Alt", EID_KeyLAlt, 80, 25),
            Gap(19),
            UIComponent::CheckBox(L"右Alt", EID_KeyRAlt, 80, 25),
        }, 0, 0, 6)
        .AddRow({
            UIComponent::CheckBox(L"左Shift", EID_KeyLShift, 86, 25),
            Gap(13),
            UIComponent::CheckBox(L"右Shift", EID_KeyRShift, 86, 25),
        }, 0, 0, 6)
        .AddRow({
            UIComponent::Label(L"循环次数", -1, 75, 22),
            Gap(16),
            UIComponent::Edit(L"0", 0, 54, 22),
        }, 0, 0, 15)
        .AddRow({
            UIComponent::Label(L"重复间隔", -1, 75, 22),
            Gap(16),
            UIComponent::Edit(L"0.010", 0, 54, 22),
            Gap(6),
            UIComponent::Label(L"秒", -1, 32, 25),
        }, 0, 0, 13)
        .AddRow({
            UIComponent::Label(L"随机间隔", -1, 75, 22),
            Gap(16),
            UIComponent::Edit(L"0.000", 0, 54, 22),
            Gap(6),
            UIComponent::Label(L"秒", -1, 32, 25),
        });
}

// ══════════════════════════════════════════════════════════════════
// 10/11. 键盘按下/松开
// ══════════════════════════════════════════════════════════════════
inline UILayout KeyPress() {
    return UILayout(kParamPanelLeft, 174, kPanelWidth)
        .AddRow({
            UIComponent::EditorLabel(L"选择键盘按键", -1, kPanelWidth, 22),
        }, 0, 0, 8)
        .AddRow({
            UIComponent::CaptureField(L"点击修改", EID_KeyPressCapture, kPanelWidth, 21),
        }, 0, 0, 11)
        .AddRow({
            UIComponent::Label(L"同时按住", -1, 120, 25),
        }, 0, 0, 6)
        .AddRow({
            UIComponent::CheckBox(L"左Win", EID_KeyPressLWin, 80, 25),
            Gap(19),
            UIComponent::CheckBox(L"右Win", EID_KeyPressRWin, 80, 25),
        }, 0, 0, 6)
        .AddRow({
            UIComponent::CheckBox(L"左Ctrl", EID_KeyPressLCtrl, 80, 25),
            Gap(19),
            UIComponent::CheckBox(L"右Ctrl", EID_KeyPressRCtrl, 80, 25),
        }, 0, 0, 6)
        .AddRow({
            UIComponent::CheckBox(L"左Alt", EID_KeyPressLAlt, 80, 25),
            Gap(19),
            UIComponent::CheckBox(L"右Alt", EID_KeyPressRAlt, 80, 25),
        }, 0, 0, 6)
        .AddRow({
            UIComponent::CheckBox(L"左Shift", EID_KeyPressLShift, 86, 25),
            Gap(13),
            UIComponent::CheckBox(L"右Shift", EID_KeyPressRShift, 86, 25),
        });
}

// ══════════════════════════════════════════════════════════════════
// 12. 快捷按键
// ══════════════════════════════════════════════════════════════════
inline UILayout HotkeyShortcut() {
    return UILayout(kParamPanelLeft, 174, kPanelWidth)
        .AddRow({
            UIComponent::EditorLabel(L"要使用的快捷按键", -1, kPanelWidth, 22),
        }, 0, 0, 8)
        .AddRow({
            UIComponent::Combo(L"Ctrl+C(拷贝)", EID_HotkeyShortcutCombo, kPanelWidth, 21),
        }, 0, 0, 11)                               // 循环/等待区紧跟下拉框
        .AddRow({
            UIComponent::Label(L"循环次数", -1, 75, 22),
            Gap(16),
            UIComponent::Edit(L"0", EID_HotkeyShortcutCount, 54, 22),
        }, 0, 0, 15)
        .AddRow({
            UIComponent::Label(L"重复间隔", -1, 75, 22),
            Gap(16),
            UIComponent::Edit(L"0.010", EID_HotkeyShortcutWait, 54, 22),
            Gap(6),
            UIComponent::Label(L"秒", -1, 32, 25),
        }, 0, 0, 13)
        .AddRow({
            UIComponent::Label(L"随机间隔", -1, 75, 22),
            Gap(16),
            UIComponent::Edit(L"0.000", EID_HotkeyShortcutRandom, 54, 22),
            Gap(6),
            UIComponent::Label(L"秒", -1, 32, 25),
        });
}

// ══════════════════════════════════════════════════════════════════
// 13. 快捷输入
// ══════════════════════════════════════════════════════════════════
inline UILayout QuickInput() {
    return UILayout(kParamPanelLeft, 174, kPanelWidth)
        .AddRow({
            UIComponent::EditorLabel(L"要输入的文字", -1, kPanelWidth, 22),
        }, 0, 0, 8)                                // y=204
        .AddRow({
            UIComponent::MultilineEdit(L"", EID_QuickInputText, kPanelWidth, kPanelTextFieldH),
        }, 0, 0, 6)                                // y=290
        .AddRow({
            UIComponent::Label(L"变量:", -1, 50, 22),
            Gap(6),
            UIComponent::Combo(L"a", EID_QuickInputVarCombo, 149, 21),
        }, 0, 0, 7)                                // y=318
        .AddRow({
            UIComponent::GreenButton(L"插入选择的变量", EID_QuickInputInsert, kPanelWidth, 28),
        }, 0, 0, 12)                               // y=358
        .AddRow({
            UIComponent::Label(L"字输入间隔", -1, 90, 22),
            Gap(1),
            UIComponent::Edit(L"0.010", EID_QuickInputCharInterval, 54, 22),
            Gap(6),
            UIComponent::Label(L"秒", -1, 32, 25),
        }, 0, 0, 6)                                // y=386
        .AddRow({
            UIComponent::Hint(L"*提示:直接输入文字或变量中的值: 使用来自变量中的", kParamFieldWidth, 56),
        }, 0, 0, 16)                               // y=458
        .AddRow({
            UIComponent::Label(L"循环次数", -1, 75, 22),
            Gap(16),
            UIComponent::Edit(L"0", EID_QuickInputCount, 54, 22),
        }, 0, 0, 10)                               // y=490
        .AddRow({
            UIComponent::Label(L"重复间隔", -1, 75, 22),
            Gap(16),
            UIComponent::Edit(L"0.010", EID_QuickInputWait, 54, 22),
            Gap(6),
            UIComponent::Label(L"秒", -1, 32, 25),
        }, 0, 0, 7)                                // y=522
        .AddRow({
            UIComponent::Label(L"随机间隔", -1, 75, 22),
            Gap(16),
            UIComponent::Edit(L"0.000", EID_QuickInputRandom, 54, 22),
            Gap(6),
            UIComponent::Label(L"秒", -1, 32, 25),
        });
}

// ══════════════════════════════════════════════════════════════════
// 14. 循环
// ══════════════════════════════════════════════════════════════════
inline UILayout Loop() {
    return UILayout(kParamPanelLeft, 174, kPanelWidth)
        .AddRow({
            UIComponent::EditorLabel(L"循环类型", -1, kPanelWidth, 22),
        }, 0, 0, 8)
        .AddRow({
            // 目前仅「次数循环」生效；类型下拉预留扩展（条件/时长等）
            UIComponent::Combo(L"次数循环", EID_LoopTypeCombo, kPanelWidth, 21),
        }, 0, 0, 19)                               // y=244
        .AddRow({
            UIComponent::Label(L"循环次数", -1, 75, 22),
            Gap(16),
            UIComponent::Edit(L"-1", EID_LoopCount, 54, 22),
        }, 0, 0, 6)                                // y=272
        .AddRow({
            UIComponent::Hint(L"*提示:-1表示无限循环；勾选下方可按变量解析次数", kParamFieldWidth, 40),
        }, 0, 0, 8)                                // y=308
        .AddRow({
            UIComponent::CheckBox(L"来自变量表达式", EID_LoopFromVar, 180, 25),
        }, 0, 0, 7)                                // y=340
        .AddRow({
            UIComponent::FieldEdit(L"", EID_LoopVarExpr, kParamFieldWidth, kPanelSingleFieldH),
        }, 0, 0, 16)                               // y=378
        .AddRow({
            UIComponent::Label(L"循环变量命名", -1, 120, 25),
        }, 0, 0, 1)                                // y=404
        .AddRow({
            UIComponent::FieldEdit(L"", EID_LoopVarName, kParamFieldWidth, kPanelSingleFieldH),
        }, 0, 0, 6)                                // y=432
        .AddRow({
            UIComponent::Hint(L"*提示:记录当前第几次循环，可用于索引/条件判断", kParamFieldWidth, 40),
        });
}

// ══════════════════════════════════════════════════════════════════
// 15. 结束循环
// ══════════════════════════════════════════════════════════════════
inline UILayout EndLoop() {
    return UILayout(kParamPanelLeft, 183, kPanelWidth)
        .AddRow({
            UIComponent::Hint(L"*提示:须放在循环内（含循环下的条件分支），用于提前结束循环", kParamFieldWidth, 48),
        });
}

// ══════════════════════════════════════════════════════════════════
// 16. 定义宏指令块
// ══════════════════════════════════════════════════════════════════
inline UILayout DefineBlock() {
    return UILayout(kParamPanelLeft, 174, kPanelWidth)
        .AddRow({
            UIComponent::EditorLabel(L"定义的宏指令块名称:", -1, kPanelWidth, 22),
        }, 0, 0, 8)
        .AddRow({
            UIComponent::FieldEdit(L"block1", EID_DefineBlockName, kPanelWidth, kPanelSingleFieldH),
        }, 0, 0, 8)
        .AddRow({
            UIComponent::Hint(
                L"*提示:块名称不能重复;\r\n块名称只能以字母开始, 后面可以加数字和字母如 'DaKai001';\r\n添加时宏模块自动添加到顶部。",
                205, 96),
        });
}

// ══════════════════════════════════════════════════════════════════
// 17. 运行宏指令块
// ══════════════════════════════════════════════════════════════════
inline UILayout RunBlock() {
    return UILayout(kParamPanelLeft, 174, kPanelWidth)
        .AddRow({
            UIComponent::EditorLabel(L"要运行的宏指令块名称:", -1, kPanelWidth, 22),
        }, 0, 0, 8)
        .AddRow({
            UIComponent::Combo(L"", EID_RunBlockCombo, kPanelWidth, 21),
        });
}

// ══════════════════════════════════════════════════════════════════
// 18-24. 找图 / 文字识别 — 使用 config.h 常量
// ══════════════════════════════════════════════════════════════════

inline UILayout FindImageBase() {
    // layout left=kFindContentLeft=785, width=kFindBlockW=218
    // action buttons (screenshot/local/clear) all at x=913 → offset within layout = 128
    // y positions:
    //   找图区域 row → y=180, end=218
    //   X1/Y1 row     → y=218, end=248
    //   X2/Y2 row     → y=248, end=278
    //   要查找的图 row → y=278, end=316
    //   preview row   → y=316, rowH=120, end=436
    //   screenshot btn at y=316 (top of preview, same row)
    //   local btn     → y=361 (centered: (346+406)/2 - 15)
    //   clear btn     → y=406 (preview bottom 436 - 30)
    //   匹配度 row    → y=448, end=478
    //   缩放 row      → y=478, end=508
    //   后续操作 row  → y=508, end=546
    return UILayout(kFindContentLeft, kFindRegionRowY, kFindBlockW, 0)  // rowGap=0
        .AddRow({
            UIComponent::Label(L"找图区域", -1, kFindRegionLabelW, kFindBtnH),
            Gap(kFindRegionLabelGap),
            UIComponent::GrayButton(L"全图", EID_FindFullScreen, 56, kFindBtnH),
            Gap(kFindRegionBtnGap),
            UIComponent::GrayButton(L"选取区域", EID_FindSelectRegion, kFindBtnW, kFindBtnH),
        }, 0, 0, kFindVGap)
        .AddRow({
            UIComponent::Label(L"X1", -1, kFindCoordLabelW, 22),
            Gap(kFindCoordLabelEditGap),
            UIComponent::FieldEdit(L"0", EID_FindX1, kFindEditW, 22),
            Gap(kFindCoordPairGap),
            UIComponent::Label(L"Y1", -1, kFindCoordLabelW, 22),
            Gap(kFindCoordLabelEditGap),
            UIComponent::FieldEdit(L"0", EID_FindY1, kFindEditW, 22),
        }, 0, 0, kFindVGap)
        .AddRow({
            UIComponent::Label(L"X2", -1, kFindCoordLabelW, 22),
            Gap(kFindCoordLabelEditGap),
            UIComponent::FieldEdit(L"0", EID_FindX2, kFindEditW, 22),
            Gap(kFindCoordPairGap),
            UIComponent::Label(L"Y2", -1, kFindCoordLabelW, 22),
            Gap(kFindCoordLabelEditGap),
            UIComponent::FieldEdit(L"0", EID_FindY2, kFindEditW, 22),
        }, 0, 0, kFindVGap)
        .AddRow({
            UIComponent::Label(L"要查找的图", -1, 90, kFindBtnH),
            Gap(38),                                         // 913 - 785 - 90 = 38
            UIComponent::GrayButton(L"测试", EID_FindTest, kFindBtnW, kFindBtnH),
        }, 0, 0, kFindVGap)
        // preview (120x120) + screenshot button (w=90) at same y=316, row height=120
        .AddRow({
            UIComponent::GrayButton(L"", EID_FindImagePreview, kFindImageSize, kFindImageSize),
            Gap(8),                                          // kFindActionBtnX - kFindContentLeft - 120 = 913-785-120 = 8
            UIComponent::GrayButton(L"屏幕截图", EID_FindScreenshot, kFindBtnW, kFindBtnH),
        }, 0, 0, 0)
        // local image button at y=361 (negative marginTop: 361 - 436 = -75)
        .AddRow({
            Gap(128),                                        // 913 - 785 = 128
            UIComponent::GrayButton(L"本地图片", EID_FindLocalImage, kFindBtnW, kFindBtnH),
        }, 0, -75, 0)
        // clear image button at y=406 (marginTop: 406 - 391 = 15)
        .AddRow({
            Gap(128),
            UIComponent::GrayButton(L"清除图片", EID_FindClearImage, kFindBtnW, kFindBtnH),
        }, 0, 15, 0)
        // match row at y=448 (marginTop: 448 - 436 = 12)
        .AddRow({
            UIComponent::Label(L"匹配度大于", -1, 90, 22),
            Gap(1),
            UIComponent::FieldEdit(L"65", EID_FindMatchThreshold, 40, 22),
            Gap(4),
            UIComponent::Label(L"%", -1, 24, 22),
        }, 0, 4, kFindVGap)
        // scale row at y=478 (marginTop=0, flows from previous)
        .AddRow({
            UIComponent::Label(L"最小缩放", -1, 64, 22),
            Gap(1),
            UIComponent::FieldEdit(L"0.9", EID_FindScaleMin, 40, 22),
            Gap(5),
            UIComponent::Label(L"最大", -1, 40, 22),
            Gap(1),
            UIComponent::FieldEdit(L"1.1", EID_FindScaleMax, 40, 22),
        }, 0, 0, kFindVGap)
        // follow-up row at y=508
        .AddRow({
            UIComponent::Label(L"后续操作", -1, kFindFollowLabelW, kFindBtnH),
            Gap(8),
            UIComponent::Combo(L"点击", EID_FindFollowUp, kFindFollowComboW, kFindBtnH),
        }, 0, 0, kFindVGap);
}

inline UILayout FindImageOffset() {
    return UILayout(kFindContentLeft, kFindOffsetRowY, kFindBlockW)
        .AddRow({
            UIComponent::Label(L"X偏", -1, kFindOffsetLabelW, 22),
            Gap(kFindCoordLabelEditGap),
            UIComponent::FieldEdit(L"0", EID_FindOffsetX, kFindEditW, 22),
            Gap(kFindCoordPairGap),
            UIComponent::Label(L"Y偏", -1, kFindOffsetLabelW, 22),
            Gap(kFindCoordLabelEditGap),
            UIComponent::FieldEdit(L"0", EID_FindOffsetY, kFindEditW, 22),
        }, 0, 0, kFindVGap)
        .AddRow({
            Gap(kFindSelectOffsetLeft - kFindContentLeft),
            UIComponent::GrayButton(L"选择偏移点击位置", EID_FindSelectOffset, kFindSelectOffsetW, kFindBtnH),
        }, 0, 0, kFindVGap)
        .AddRow({
            UIComponent::Label(L"时间", -1, 44, 22),
            Gap(4),
            UIComponent::FieldEdit(L"0", EID_FindTime, kFindBlockW - 48, 22),
        }, 0, 0, kFindVGap);
}

inline UILayout FindImageVar() {
    return UILayout(kFindContentLeft, kFindOffsetRowY, kFindBlockW)
        .AddRow({
            UIComponent::Label(L"匹配度保存到", -1, kFindMatchVarLabelW, 22),
            Gap(4),
            UIComponent::FieldEdit(L"matchRet", EID_FindMatchVar, kFindMatchVarEditW, 22),
        });
}

// ── 文字识别 ──

inline UILayout OcrDepStatus() {
    return UILayout(kFindContentLeft, kOcrDepRowY, kFindBlockW)
        .AddRow({
            UIComponent::Label(L"文字识别未安装", -1, 120, kFindBtnH),
            Gap(8),
            UIComponent::GrayButton(L"一键安装", EID_OcrInstallDep, kFindBtnW, kFindBtnH),
        });
}

inline UILayout OcrFindRegionToggle() {
    return UILayout(kFindContentLeft, kOcrToggleRowY, kFindBlockW)
        .AddRow({
            UIComponent::CheckBox(L"根据图片选取区域", EID_OcrRegionByImage, kFindBlockW, 22),
        }, 0, 0, kFindVGap)
        .AddRow({
            UIComponent::CheckBox(L"纯数字", EID_OcrDigitsOnly, kFindBlockW, 22),
        });
}

inline UILayout OcrBase() {
    return UILayout(kFindContentLeft, kOcrRegionRowY, kFindBlockW)
        .AddRow({
            UIComponent::Label(L"识别区域", -1, kFindRegionLabelW, kFindBtnH),
            Gap(kFindRegionLabelGap),
            UIComponent::GrayButton(L"全图", EID_OcrFullScreen, 44, kFindBtnH),
            Gap(kFindRegionBtnGap),
            UIComponent::GrayButton(L"选取区域", EID_OcrSelectRegion, kFindBtnW, kFindBtnH),
        }, 0, 0, kFindVGap)
        .AddRow({
            UIComponent::Label(L"X1", -1, kFindCoordLabelW, 22),
            Gap(kFindCoordLabelEditGap),
            UIComponent::FieldEdit(L"0", EID_OcrX1, kFindEditW, 22),
            Gap(kFindCoordPairGap),
            UIComponent::Label(L"Y1", -1, kFindCoordLabelW, 22),
            Gap(kFindCoordLabelEditGap),
            UIComponent::FieldEdit(L"0", EID_OcrY1, kFindEditW, 22),
        }, 0, 0, kFindVGap)
        .AddRow({
            UIComponent::Label(L"X2", -1, kFindCoordLabelW, 22),
            Gap(kFindCoordLabelEditGap),
            UIComponent::FieldEdit(L"0", EID_OcrX2, kFindEditW, 22),
            Gap(kFindCoordPairGap),
            UIComponent::Label(L"Y2", -1, kFindCoordLabelW, 22),
            Gap(kFindCoordLabelEditGap),
            UIComponent::FieldEdit(L"0", EID_OcrY2, kFindEditW, 22),
        }, 0, 0, kFindVGap)
        .AddRow({
            UIComponent::Label(L"结果处理", -1, kFindFollowLabelW, kFindBtnH),
            Gap(8),
            UIComponent::Combo(L"获取文字", EID_OcrResultMode, kFindFollowComboW, kFindBtnH),
        }, 0, 0, kFindVGap)
        .AddRow({
            UIComponent::GrayButton(L"测试", EID_OcrTest, kFindBtnW, kFindBtnH),
        }, 0, 0, kFindVGap);
}

inline UILayout OcrSearch() {
    return UILayout(kFindContentLeft, kOcrSearchLabelY, kFindBlockW)
        .AddRow({
            UIComponent::Label(L"查找:", -1, kOcrSearchVarLabelW, 22),
            Gap(kOcrVarComboGap),
            UIComponent::MultilineEdit(L"", EID_OcrSearchText, kOcrSearchEditW, kPanelTextFieldH),
        }, 0, 0, 4)
        .AddRow({
            UIComponent::Label(L"变量:", -1, kOcrSearchVarLabelW, 22),
            Gap(kOcrVarComboGap),
            UIComponent::Combo(L"", EID_OcrSearchVarCombo, kFindFollowComboW, kFindBtnH),
        }, 0, 0, 4)
        .AddRow({
            UIComponent::Button(L"插入", EID_OcrSearchVarInsert, kOcrInsertBtnW, kOcrCompactBtnH),
        });
}

inline UILayout OcrFollowUp() {
    return UILayout(kFindContentLeft, kOcrFollowRowY, kFindBlockW)
        .AddRow({
            UIComponent::Label(L"后续操作", -1, kFindFollowLabelW, kFindBtnH),
            Gap(8),
            UIComponent::Combo(L"点击", EID_OcrFollowUp, kFindFollowComboW, kFindBtnH),
        }, 0, 0, kFindVGap)
        .AddRow({
            UIComponent::CheckBox(L"直到找到为止", EID_OcrUntilFound, 140, 22),
        }, 0, 0, kFindVGap);
}

inline UILayout OcrFollowOffset() {
    return UILayout(kFindContentLeft, kOcrOffsetRowY, kFindBlockW)
        .AddRow({
            UIComponent::Label(L"X偏", -1, kFindOffsetLabelW, 22),
            Gap(kFindCoordLabelEditGap),
            UIComponent::FieldEdit(L"0", EID_OcrOffsetX, kFindEditW, 22),
            Gap(kFindCoordPairGap),
            UIComponent::Label(L"Y偏", -1, kFindOffsetLabelW, 22),
            Gap(kFindCoordLabelEditGap),
            UIComponent::FieldEdit(L"0", EID_OcrOffsetY, kFindEditW, 22),
        }, 0, 0, kFindVGap)
        .AddRow({
            Gap(kFindSelectOffsetLeft - kFindContentLeft),
            UIComponent::GrayButton(L"选择偏移位置", EID_OcrSelectOffset, kFindSelectOffsetW, kFindBtnH),
        }, 0, 0, kFindVGap);
}

inline UILayout OcrFollowVar() {
    return UILayout(kFindContentLeft, kOcrResultVarY, kFindBlockW)
        .AddRow({
            UIComponent::Label(L"结果保存到", -1, 100, 22),
            Gap(1),
            UIComponent::MultilineEdit(L"a", EID_OcrResultVar, kOcrResultVarEditW, kPanelTextFieldH),
        });
}

inline UILayout OcrFindRegion() {
    return UILayout(kFindContentLeft, kFindImageLabelY, kFindBlockW)
        .AddRow({
            UIComponent::Label(L"要查找的图", -1, 90, kFindBtnH),
            Gap(kFindActionBtnX - kFindContentLeft - 90),
            UIComponent::GrayButton(L"选取区域", EID_OcrFindSelectRegion, kFindBtnW, kFindBtnH),
        }, 0, 0, kFindVGap)
        .AddRow({
            UIComponent::GrayButton(L"", EID_OcrFindImagePreview, kFindImageSize, kFindImageSize),
            Gap(kFindContentGap),
            UIComponent::GrayButton(L"屏幕截图", EID_OcrFindScreenshot, kFindBtnW, kFindBtnH),
        }, 0, 0, 0)
        .AddRow({
            Gap(kFindActionBtnX - kFindContentLeft),
            UIComponent::GrayButton(L"本地图片", EID_OcrFindLocalImage, kFindBtnW, kFindBtnH),
        }, 0, 0, 0)
        .AddRow({
            Gap(kFindActionBtnX - kFindContentLeft),
            UIComponent::GrayButton(L"清除图片", EID_OcrFindClearImage, kFindBtnW, kFindBtnH),
        }, 0, 0, kFindVGap)
        .AddRow({
            UIComponent::Label(L"匹配度大于", -1, 90, 22),
            Gap(1),
            UIComponent::FieldEdit(L"65", EID_OcrFindMatchThreshold, 40, 22),
            Gap(4),
            UIComponent::Label(L"%", -1, 24, 22),
        }, 0, 0, kFindVGap)
        .AddRow({
            UIComponent::Label(L"最小缩放", -1, 64, 22),
            Gap(1),
            UIComponent::FieldEdit(L"0.9", EID_OcrFindScaleMin, 40, 22),
            Gap(5),
            UIComponent::Label(L"最大", -1, 40, 22),
            Gap(1),
            UIComponent::FieldEdit(L"1.1", EID_OcrFindScaleMax, 40, 22),
        });
}

// ══════════════════════════════════════════════════════════════════
// 25. 条件-如果
// ══════════════════════════════════════════════════════════════════
inline UILayout IfCondition() {
    return UILayout(kParamPanelLeft, 180, kPanelWidth)
        .AddRow({
            UIComponent::Label(L"如果:", -1, 60, 22),
        }, 0, 0, 12)                               // y=214
        .AddRow({
            UIComponent::Label(L"变量:", -1, 50, 22),
            Gap(6),
            UIComponent::Combo(L"", EID_IfVarCombo, 149, 21),
        }, 0, 0, 12)                               // y=248
        .AddRow({
            UIComponent::Label(L"判断:", -1, 50, 22),
            Gap(6),
            UIComponent::Combo(L"等于", EID_IfOperator, 149, 21),
        }, 0, 0, 12)                               // y=282
        .AddRow({
            UIComponent::Label(L"值:", -1, 50, 22),
            Gap(6),
            UIComponent::Edit(L"0", EID_IfValue, 149, 22),
        }, 0, 0, 12)                               // y=316
        .AddRow({
            UIComponent::Label(L"多条件连接方式:", -1, 120, 22),
        }, 0, 0, 8)                                // y=346
        .AddRow({
            UIComponent::Combo(L"并且(and)", EID_IfConnector, kPanelWidth, 21),
        }, 0, 0, 11)                               // y=378
        .AddRow({
            UIComponent::GreenButton(L"添加判断条件", EID_IfAddCondition, kPanelWidth, 28),
        }, 0, 0, 6)                                // y=412
        .AddRow({
            UIComponent::Label(L"判断条件:", -1, 80, 22),
        }, 0, 0, 2)                                // y=436
        .AddRow({
            UIComponent::MultilineEdit(L"", EID_IfConditionList, kPanelWidth, kPanelTextFieldH),
        }, 0, 0, 4)                                // y=512
        .AddRow({
            UIComponent::Hint(L"*提示:如需要更复杂的条件判断，请导出脚本操作", kPanelWidth, 28),
        });
}

// ══════════════════════════════════════════════════════════════════
// 26-29. 其他简单面板 (全部是 hint)
// ══════════════════════════════════════════════════════════════════

inline UILayout ElseCondition() {
    return UILayout(kParamPanelLeft, 183, kPanelWidth)
        .AddRow({
            UIComponent::Hint(L"*提示:必须和如果成对出现，作为如果节点的下方兄弟节点 (非子节点)！", kParamFieldWidth, 48),
        });
}

inline UILayout LockScreenshot() {
    return UILayout(kParamPanelLeft, 183, kPanelWidth)
        .AddRow({
            UIComponent::Hint(L"*提示:锁定屏幕截图，锁定后后续的找图、找色、颜色匹配、文字识别不会再次截图，而是使用锁定时的屏幕截图进行处理", kParamFieldWidth, 96),
        });
}

inline UILayout UnlockScreenshot() {
    return UILayout(kParamPanelLeft, 183, kPanelWidth)
        .AddRow({
            UIComponent::Hint(L"*提示:解锁定屏幕截图，与锁定截屏成对出现，取消锁定后找图、找色、颜色匹配、文字识别每次使用都会再次截图进行处理", kParamFieldWidth, 96),
        });
}

inline UILayout StopMacro() {
    return UILayout(kParamPanelLeft, 183, kPanelWidth)
        .AddRow({
            UIComponent::Hint(L"*提示:会结束宏的运行，与您按下快捷键停止是一样的", kParamFieldWidth, 48),
        });
}

inline UILayout Goto() {
    return UILayout(kParamPanelLeft, 174, kPanelWidth)
        .AddRow({
            UIComponent::EditorLabel(L"跳转到动作序号", -1, kPanelWidth, 22),
        }, 0, 0, 8)
        .AddRow({
            UIComponent::FieldEdit(L"", EID_GotoStepExpr, kParamFieldWidth, kPanelSingleFieldH),
        }, 0, 0, 8)
        .AddRow({
            UIComponent::Hint(
                L"*提示:填写目标动作的序号（列表左侧序号），支持变量名或 {变量} 表达式\r\n"
                L"跳入循环体内=该循环第1次迭代并从目标动作执行；循环内互跳保持同次迭代",
                kParamFieldWidth, 64),
        });
}

// ══════════════════════════════════════════════════════════════════
// 30. 打开程序
// ══════════════════════════════════════════════════════════════════
inline UILayout RunProgram() {
    return UILayout(kParamPanelLeft, 174, kPanelWidth)
        .AddRow({
            UIComponent::EditorLabel(L"要运行的程序:", -1, kPanelWidth, 22),
        }, 0, 0, 8)
        .AddRow({
            UIComponent::Combo(L"选择文件", EID_RunProgramCombo, kPanelWidth, 21),
        });
}

inline UILayout RunProgramFile() {
    return UILayout(kParamPanelLeft, 232, kPanelWidth)                 // starts at y=232 (combo ends at 225+gap=7→232)
        .AddRow({
            UIComponent::MultilineEdit(L"", EID_RunProgramPath, kPanelWidth, kPanelTextFieldH),
        }, 0, 0, 8)                                // y=304
        .AddRow({
            UIComponent::GreenButton(L"选择要启动的程序", EID_RunProgramBrowse, kPanelWidth, 28),
        }, 0, 0, 4)                                // y=336, label "或" at y=340
        .AddRow({
            UIComponent::Label(L"或", -1, 20, 22),
        }, 0, 0, -26)                              // negative margin to overlay with green button
                                                    // The green button was at y=336 in original, overlap with "或" at 340
        .AddRow({
            UIComponent::GreenButton(L"拖动准星查找程序", EID_RunProgramCrosshair, 181, 32),
        }, 0, 0, 10)                               // y=378
        .AddRow({
            UIComponent::Label(L"运行参数", -1, kPanelWidth, 22),
        }, 0, 0, 4)                                // y=404
        .AddRow({
            UIComponent::MultilineEdit(L"", EID_RunProgramArgs, kPanelWidth, kPanelTextFieldH),
        });
}

// ══════════════════════════════════════════════════════════════════
// 31. 关闭程序
// ══════════════════════════════════════════════════════════════════
inline UILayout CloseProgram() {
    return UILayout(kParamPanelLeft, 174, kPanelWidth)
        .AddRow({
            UIComponent::EditorLabel(L"要关闭的程序:", -1, kPanelWidth, 22),
        }, 0, 0, 8)                                // y=204
        .AddRow({
            UIComponent::MultilineEdit(L"", EID_CloseProgramPath, kPanelWidth, kPanelTextFieldH),
        }, 0, 0, 8)                                // y=276
        .AddRow({
            UIComponent::Button(L"选择要关闭的程序", EID_CloseProgramBrowse, kPanelWidth, 28),
        }, 0, 0, 4)                                // y=308, overlap with "或" at 312
        .AddRow({
            UIComponent::Label(L"或", -1, 20, 22),
        }, 0, 0, -26)
        .AddRow({
            UIComponent::GreenButton(L"拖动准星查找程序", EID_CloseProgramCrosshair, 181, 32),
        }, 0, 0, 10)                               // y=350
        .AddRow({
            UIComponent::CheckBox(L"匹配全路径", EID_CloseProgramMatchFileName, 120, 25),
        }, 0, 0, 8)                                // y=383
        .AddRow({
            UIComponent::Hint(L"*提示:选中只匹配文件名，否则匹配全路径", kPanelWidth, 40),
        });
}

// ══════════════════════════════════════════════════════════════════
// 32. 打开网页
// ══════════════════════════════════════════════════════════════════
inline UILayout OpenWebpage() {
    return UILayout(kParamPanelLeft, 174, kPanelWidth)
        .AddRow({
            UIComponent::EditorLabel(L"要打开的网页:", -1, kPanelWidth, 22),
        }, 0, 0, 8)
        .AddRow({
            UIComponent::MultilineEdit(L"", EID_OpenWebpageUrl, kPanelWidth, kPanelTextFieldH),
        }, 0, 0, 8)
        .AddRow({
            UIComponent::Hint(L"*提示：可仅支持http:// https://开头的网址", kPanelWidth, 40),
        });
}

// ══════════════════════════════════════════════════════════════════
// 33. 打开文件
// ══════════════════════════════════════════════════════════════════
inline UILayout OpenFile() {
    return UILayout(kParamPanelLeft, 174, kPanelWidth)
        .AddRow({
            UIComponent::EditorLabel(L"要打开的文件:", -1, kPanelWidth, 22),
        }, 0, 0, 8)
        .AddRow({
            UIComponent::MultilineEdit(L"", EID_OpenFilePath, kPanelWidth, kPanelTextFieldH),
        }, 0, 0, 6)
        .AddRow({
            UIComponent::Button(L"选择要打开的文件", EID_OpenFileBrowse, kPanelWidth, 28),
        }, 0, 0, 8)
        .AddRow({
            UIComponent::Hint(L"*提示：请确保文件扩展名已关联默认程序，否则可能出现提示窗口，导致打开文件失败", kPanelWidth, 48),
        });
}

// ══════════════════════════════════════════════════════════════════
// 34. 计时器
// ══════════════════════════════════════════════════════════════════
inline UILayout TimerRecord() {
    return UILayout(kParamPanelLeft, 174, kPanelWidth)
        .AddRow({
            UIComponent::EditorLabel(L"计时器变量命名", -1, kPanelWidth, 22),
        }, 0, 0, 8)
        .AddRow({
            UIComponent::FieldEdit(L"", EID_TimerVarName, kPanelWidth, kPanelSingleFieldH),
        }, 0, 0, 8)
        .AddRow({
            UIComponent::Hint(L"*提示: 记录从此动作到引用该变量时经过的秒数(向下取整); 同名变量会重置计时", kPanelWidth, 56),
        });
}

// ══════════════════════════════════════════════════════════════════
// 35. 获取当前光标位置
// ══════════════════════════════════════════════════════════════════
inline UILayout GetCursorPos() {
    return UILayout(kParamPanelLeft, 174, kPanelWidth)
        .AddRow({
            UIComponent::Label(L"变量命名", -1, 120, 25),
        }, 0, 0, 1)
        .AddRow({
            UIComponent::FieldEdit(L"", EID_CursorPosVarName, kParamFieldWidth, kPanelSingleFieldH),
        }, 0, 0, 6)
        .AddRow({
            UIComponent::Hint(
                L"*提示: 保存当前光标坐标\r\n变量名为 a 时可用 {a.x}、{a.y}",
                kParamFieldWidth, 56),
        }, 0, 0, 16);
}

// ══════════════════════════════════════════════════════════════════
// 36-40. AI 面板 (暂按原有设计)
// ══════════════════════════════════════════════════════════════════
inline UILayout AiCommon() {
    return UILayout(kParamPanelLeft, 174, kPanelWidth)
        .AddRow({
            UIComponent::EditorLabel(L"提示词 (Prompt)", -1, kPanelWidth - 52 - 8, 22),
            Gap(8),
            UIComponent::CheckBox(L"图片", EID_AiWithImage, 52, 25),
        }, 0, 0, 8)
        .AddRow({
            UIComponent::MultilineEdit(L"", EID_AiPrompt, kPanelWidth, kPanelTextFieldH),
        }, 0, 0, 8)
        .AddRow({
            UIComponent::EditorLabel(L"最大步骤数", -1, kPanelWidth, 22),
        }, 0, 0, 6)
        .AddRow({
            UIComponent::Edit(L"10", EID_AiMaxSteps, kPanelWidth, 22),
        }, 0, 0, 4)
        .AddRow({
            UIComponent::Hint(L"*提示:-1表示不限制步数", kPanelWidth, 28),
        }, 0, 0, 8)
        .AddRow({
            UIComponent::EditorLabel(L"变量", -1, kPanelWidth, 22),
        }, 0, 0, 6)
        .AddRow({
            UIComponent::Combo(L"a", EID_AiVarCombo, kPanelWidth, 21),
        }, 0, 0, 8)
        .AddRow({
            UIComponent::GreenButton(L"插入选择的变量", EID_AiInsertVar, kPanelWidth, 28),
        }, 0, 0, 12)
        .AddRow({
            UIComponent::EditorLabel(L"AI 模型", -1, kPanelWidth, 22),
        }, 0, 0, 6)
        .AddRow({
            UIComponent::Combo(L"", EID_AiModel, kPanelWidth, 21),
        }, 0, 0, 8)
        .AddRow({
            UIComponent::EditorLabel(L"上下文模式", -1, kPanelWidth, 22),
        }, 0, 0, 6)
        .AddRow({
            UIComponent::Combo(L"无上下文", EID_AiContextMode, kPanelWidth, 21),
        }, 0, 0, 8)
        .AddRow({
            UIComponent::EditorLabel(L"超时(秒)", -1, kPanelWidth, 22),
        }, 0, 0, 6)
        .AddRow({
            UIComponent::Edit(L"30", EID_AiTimeout, kPanelWidth, 22),
        }, 0, 0, 8)
        .AddRow({
            UIComponent::EditorLabel(L"降级值(失败时使用)", -1, kPanelWidth, 22),
        }, 0, 0, 6)
        .AddRow({
            UIComponent::FieldEdit(L"", EID_AiFallback, kPanelWidth, kPanelSingleFieldH),
        }, 0, 0, 8)
        .AddRow({
            UIComponent::EditorLabel(L"输出变量名", -1, kPanelWidth, 22),
        }, 0, 0, 6)
        .AddRow({
            UIComponent::FieldEdit(L"aiResult", EID_AiOutputVar, kPanelWidth, kPanelSingleFieldH),
        }, 0, 0, 8)
        .AddRow({
            UIComponent::EditorLabel(L"输出类型", -1, kPanelWidth, 22),
        }, 0, 0, 6)
        .AddRow({
            UIComponent::Combo(L"文本", EID_AiOutputType, kPanelWidth, 21),
        });
}

inline UILayout AiImage() {
    return UILayout(kParamPanelLeft, 174, kPanelWidth)
        .AddRow({
            UIComponent::EditorLabel(L"截屏缩放(0.1-1.0)", -1, kPanelWidth, 22),
        }, 0, 0, 8)
        .AddRow({
            UIComponent::FieldEdit(L"1.0", EID_AiImageScale, kPanelWidth, kPanelSingleFieldH),
        }, 0, 0, 8)
        .AddRow({
            UIComponent::CheckBox(L"根据图片选取区域", EID_AiRegionByImage, kPanelWidth, 25),
        }, 0, 0, 8)
        .AddRow({
            UIComponent::Label(L"识别区域", -1, 60, 22),
            Gap(6),
            UIComponent::GrayButton(L"全图", EID_AiFullScreen, 44, 22),
            Gap(6),
            UIComponent::GrayButton(L"选取区域", EID_AiSelectRegion, 90, 22),
        }, 0, 0, 8)
        .AddRow({
            UIComponent::Label(L"X1", -1, 22, 22),
            Gap(6),
            UIComponent::FieldEdit(L"0", EID_AiSearchX1, 54, 22),
            Gap(10),
            UIComponent::Label(L"Y1", -1, 22, 22),
            Gap(6),
            UIComponent::FieldEdit(L"0", EID_AiSearchY1, 54, 22),
        }, 0, 0, 8)
        .AddRow({
            UIComponent::Label(L"X2", -1, 22, 22),
            Gap(6),
            UIComponent::FieldEdit(L"0", EID_AiSearchX2, 54, 22),
            Gap(10),
            UIComponent::Label(L"Y2", -1, 22, 22),
            Gap(6),
            UIComponent::FieldEdit(L"0", EID_AiSearchY2, 54, 22),
        });
}

inline UILayout AiAction() {
    return UILayout(kParamPanelLeft, 174, kPanelWidth)
        .AddRow({
            UIComponent::CheckBox(L"根据图片选取区域", EID_AiRegionByImage2, kPanelWidth, 25),
        }, 0, 0, 8)
        .AddRow({
            UIComponent::Label(L"识别区域", -1, 60, 22),
            Gap(6),
            UIComponent::GrayButton(L"全图", EID_AiFullScreen, 44, 22),
            Gap(6),
            UIComponent::GrayButton(L"选取区域", EID_AiSelectRegion, 90, 22),
        }, 0, 0, 8)
        .AddRow({
            UIComponent::Label(L"X1", -1, 22, 22),
            Gap(6),
            UIComponent::FieldEdit(L"0", EID_AiSearchX1, 54, 22),
            Gap(10),
            UIComponent::Label(L"Y1", -1, 22, 22),
            Gap(6),
            UIComponent::FieldEdit(L"0", EID_AiSearchY1, 54, 22),
        }, 0, 0, 8)
        .AddRow({
            UIComponent::Label(L"X2", -1, 22, 22),
            Gap(6),
            UIComponent::FieldEdit(L"0", EID_AiSearchX2, 54, 22),
            Gap(10),
            UIComponent::Label(L"Y2", -1, 22, 22),
            Gap(6),
            UIComponent::FieldEdit(L"0", EID_AiSearchY2, 54, 22),
        }, 0, 0, 8)
        .AddRow({
            UIComponent::CheckBox(L"确认后执行(安全模式)", EID_AiConfirm, 200, 25),
        });
}

inline UILayout AiFindRegion() {
    return UILayout(kFindContentLeft, 174, kFindBlockW)
        .AddRow({
            UIComponent::Label(L"要查找的图", -1, 90, 22),
            Gap(8),
            UIComponent::GrayButton(L"选取区域", EID_AiFindSelectRegion, 90, 22),
        }, 0, 0, 8)
        .AddRow({
            UIComponent::GrayButton(L"", EID_AiTargetPreview, 120, 120),
            Gap(8),
            UIComponent::GrayButton(L"屏幕截图", EID_AiTargetScreenshot, 90, 28),
        }, 0, 0, 0)
        .AddRow({
            Indent(798 + 128),  // right of the preview button
            UIComponent::GrayButton(L"本地图片", EID_AiTargetLocal, 90, 28),
        }, 0, 0, 0)
        .AddRow({
            Indent(798 + 128),
            UIComponent::GrayButton(L"清除图片", EID_AiTargetClear, 90, 28),
        }, 0, 0, 8)
        .AddRow({
            UIComponent::Label(L"匹配度大于", -1, 90, 22),
            Gap(1),
            UIComponent::FieldEdit(L"65", EID_AiFindMatchThreshold, 40, 22),
            Gap(4),
            UIComponent::Label(L"%", -1, 24, 22),
        }, 0, 0, 8)
        .AddRow({
            UIComponent::Label(L"最小缩放", -1, 64, 22),
            Gap(1),
            UIComponent::FieldEdit(L"0.9", EID_AiFindScaleMin, 40, 22),
            Gap(5),
            UIComponent::Label(L"最大", -1, 40, 22),
            Gap(1),
            UIComponent::FieldEdit(L"1.1", EID_AiFindScaleMax, 40, 22),
        });
}

}  // namespace EditorParamLayout
