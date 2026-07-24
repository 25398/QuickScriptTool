#include "agent_reference.h"

#include "action_utils.h"
#include "utils.h"



#include <nlohmann/json.hpp>

#include <cwctype>



namespace {



using json = nlohmann::json;



const wchar_t* kRefFormat = LR"(【脚本文件格式】

脚本是 UTF-8 JSON，位于 scripts 目录（或 recordings 录制目录）。

顶层字段：

  scriptName, recordTime, durationSeconds, hotkeyText, hotkeyVk, hotkeyModifiers, hotkeyHold,
  coordMeta, windowMode, breakoutTimeSeconds, actions[]

录制文件还可包含：
  recordingCaptureMode  -1/缺失=旧文件，0=自动，1=桌面绝对，2=FPS相对
  inputTimingVersion    1=整数微秒绝对时间轴语义

breakoutTimeSeconds（脱离时间，仅默认模式生效）：
  数字，单位秒。0 或未填写视为禁用。
  运行中若用户操作鼠标/键盘（非热键），宏会暂停并在该秒数后从当前步骤重试；
  等待期间再有用户操作会重置计时。窗口模式/后台窗口模式忽略此字段。

coordMeta（坐标元数据）：
  顶层对象，记录录制/保存时的参考分辨率。AI 生成脚本时无需手写此字段，
  调用 buildScriptActions 工具会自动设置。
  {
    "version": 1,
    "space": "screenVirtual",
    "refWidth": 2560,
    "refHeight": 1440,
    "refDpi": 96
  }

坐标系统：
  所有坐标字段（x, y, searchX1–Y2, offsetX/Y, aiSearchX1–Y2 等）均为 0.0–1.0 归一化浮点数。
  执行时自动按实际分辨率缩放，确保跨分辨率兼容。
  像素坐标 = round(norm * 当前宽度/高度)。

每个 action 对象包含公共字段 + 类型专用字段。软件保存时会写出全部字段；

AI 生成时至少必须写出 type 及该类型所需字段，其余可填默认值（见各动作说明）。



公共字段（每个 action 都有）：

  type        动作类型字符串（见下文）

  remark      备注（步骤说明、测试提示写这里；禁止用 text 覆盖动作名）

  indent      缩进层级：0=顶层；if/loop/defineBlock 的子动作 = 父级 indent+1

  no / text   由 buildScriptActions / createMacroScript 自动生成，禁止手写



缩进规则：

  loop / if / else / defineBlock 是容器，其内部动作 indent 比容器大 1。

  endLoop 必须作为 loop 的子节点（indent = loop.indent + 1），用于提前跳出循环。

  else 与对应 if 同级（indent 相同）。

★ 生成动作：必须调用 buildScriptActions 工具，禁止手写 action 对象。
  工具会自动补全全部字段，逻辑与编辑器手动添加完全一致。
)";



const wchar_t* kRefFindImage = LR"(【找图 findImage — 必读】

type 必须为 "findImage"。



★ 后续操作 findImageFollowUp（整数，决定找图之后做什么）：

  0 = 点击（默认）— 找到后在匹配中心+偏移处左键点击

  1 = 鼠标移动到 — 找到后只移动鼠标，不点击

  2 = 保存到变量 — 只把结果写入变量，不点击也不移动



★ 常见错误：用户要求「预留/保存图片变量」时，必须设 findImageFollowUp=2。

  仅写 matchVarName 而不设 findImageFollowUp=2，软件会当作「点击」执行，变量不会被正确预留。



专用字段：

  imagePath         图片路径，格式 images\xxx.bmp（相对 scripts 目录）

  matchThreshold    匹配阈值 1~100，默认 65

  searchFullScreen  1=全屏搜索，0=区域搜索（此时需 searchX1/Y1/X2/Y2）

  searchX1/Y1/X2/Y2 搜索区域坐标（searchFullScreen=0 时有效）

  imageScaleMin     最小缩放，默认 1.0

  imageScaleMax     最大缩放，默认 1.0

  imageScale        缩放中值，通常 (min+max)/2

  findImageFollowUp 后续操作：0点击 / 1移动 / 2保存变量

  offsetX, offsetY  点击/移动时的偏移（followUp=0或1 时有效）

  findTimeExpr      找图时限（秒，可小数）。0=只找一次；负数（如 -1）=直到找到；可填变量名

  findUntilFound    OCR 文字查找：1=循环直到找到（找图动作请用 findTimeExpr）

  matchVarName      变量名（followUp=2 时必须指定有意义的名字）



变量名规则：英文字母开头，仅含字母和数字，如 btnIcon、loginBtn、matchRet。

  默认名 matchRet；同一脚本中不同找图变量应用不同名字。



保存到变量（followUp=2）后可用属性（在条件/坐标表达式中引用）：

  变量名.matchData  匹配度（整数 0~100；未找到或未达阈值为 0）

  变量名.x          匹配区域左上角 X

  变量名.y          匹配区域左上角 Y

  变量名.x1         匹配区域右下角 X

  变量名.y1         匹配区域右下角 Y



判断是否找到：条件写 变量名.matchData > 0  或  变量名.matchData >= 65



★ 标准模板 — 找图保存变量 + 条件判断：

{

  "type": "findImage",

  "remark": "", "indent": 0,

  "imagePath": "images\\target.bmp",

  "matchThreshold": 65,

  "searchFullScreen": 1,

  "searchX1": 0, "searchY1": 0, "searchX2": 0, "searchY2": 0,

  "imageScale": 1, "imageScaleMin": 1, "imageScaleMax": 1,

  "findImageFollowUp": 2,

  "offsetX": 0, "offsetY": 0,

  "findTimeExpr": "0",

  "matchVarName": "btnIcon",

  ...其余公共字段填默认值...

},

{

  "type": "if",

  "remark": "", "indent": 0,

  "conditionExpr": "btnIcon.matchData > 0",

  ...其余公共字段填默认值...

},

{ "type": "mouseClick", "indent": 1, ... },

{ "type": "else", "indent": 0, ... },

{ "type": "wait", "indent": 1, "duration": 1, ... }



★ 标准模板 — 找图保存变量 + 偏移移动 + 点击：

  findImage(followUp=2, matchVarName="anchor")

  moveMouse(moveFromVar=1, moveVarExprX="anchor.x+50", moveVarExprY="anchor.y+20")

  mouseClick

)";



const wchar_t* kRefTextRecognition = LR"(【文字识别 textRecognition】

type 必须为 "textRecognition"。



ocrResultMode（识别模式）：

  0 = 获取文字 — 变量存 OCR 文本字符串

  1 = 文字查找 — 变量存是否找到(0/1)及坐标



ocrFollowUp（后续操作，与找图同理）：

  0 = 点击

  1 = 鼠标移动到

  2 = 保存到变量（只保存结果，不点击不移动）



专用字段：

  matchVarName      变量名（文字识别默认 "a"，应改为有意义的名字）

  ocrResultMode     0获取文字 / 1文字查找

  ocrFollowUp       0点击 / 1移动 / 2保存变量

  ocrSearchText     文字查找目标（mode=1 时必填，可含变量）

  ocrRegionByImage  1=根据找图锚点确定 OCR 区域（需 imagePath + 相对 searchX1~Y2）

  ocrDigitsOnly     1=纯数字模式

  searchFullScreen  全屏 OCR（非锚定模式）

  searchX1/Y1/X2/Y2 OCR 区域

  imagePath         锚定找图用的图片（ocrRegionByImage=1 时）

  matchThreshold    锚定找图阈值

  offsetX, offsetY  点击/移动偏移

  findUntilFound    文字查找模式下循环直到找到



变量引用：

  获取文字(mode=0)：{变量名} 为识别文本字符串

  文字查找(mode=1)：{变量名} 为 0/1；{变量名}.x/.y/.x1/.y1 为坐标



★ 保存 OCR 变量时必须 ocrFollowUp=2（与找图 followUp=2 同理）。

)";


const wchar_t* kRefWindowMode = LR"(【脚本模式 windowMode — 脚本头字段】

脚本 JSON 根对象与 actions 并列，保存/导入/导出/热键运行均读取此字段。
编辑界面三种模式对应关系：

  默认模式        windowMode.enabled = 0
  窗口模式        enabled = 1, executionKind = "hiddenDesktop"
  后台窗口模式    enabled = 1, executionKind = "backgroundWindow"

createMacroScript 可用 scriptMode 简写：default / window / backgroundWindow；
或传完整 windowMode 对象（优先级更高）。
默认模式可传 breakoutTimeSeconds（秒，0=禁用脱离）。

键鼠录制（recordings）始终强制默认模式，忽略 windowMode，breakoutTimeSeconds 恒为 0。

字段说明：

  windowMode.enabled              0=默认模式，1=启用窗口类模式
  windowMode.executionKind        hiddenDesktop | backgroundWindow
  windowMode.targetExePath        目标程序路径、文档路径（.txt/.xlsx 等）或 URL（http/https）；窗口不存在时用于自动打开
  windowMode.targetWindowTitle    目标窗口标题过滤
  windowMode.windowName           窗口名称（与 targetWindowTitle 同步）
  windowMode.windowClassName      窗口类名（后台模式常用）
  windowMode.childWindowClassName 子窗口类名
  windowMode.selectMethod         selectOnStartup | mousePositionOnStartup | useEditorWindowClass | noSelect
  windowMode.targetPickX/Y        准星绑定坐标
  windowMode.coordSpace           windowClient（默认）| screenAbsolute
  windowMode.autoLaunchTarget     1=运行前自动启动目标程序
  windowMode.launchArgs           启动参数
  windowMode.allowForegroundInputFallback  后台输入失败时是否抢焦点（默认 0）
  windowMode.fakeFocusEnabled              窗口模式(宏桌面)游戏假焦点实验开关（默认 0；进程注入，勿用于联机/反作弊；CDP 策略下忽略）
  windowMode.inputStrategy                 auto | softMessage | cdp（Chrome/Edge 网页会自动兼容：必要时重启浏览器并恢复标签，用户无需手动开调试）
  windowMode.cdpPort                       CDP 远程调试端口（默认 9222）

示例（后台窗口模式）：

  "windowMode": {
    "enabled": 1,
    "executionKind": "backgroundWindow",
    "windowClassName": "Notepad",
    "selectMethod": "useEditorWindowClass",
    "coordSpace": "windowClient",
    "autoLaunchTarget": 0,
    "targetExePath": "",
    "targetWindowTitle": "",
    "windowName": "",
    "childWindowClassName": "",
    "useTopLevelWindow": 1,
    "targetPickX": 0,
    "targetPickY": 0,
    "launchArgs": "",
    "allowForegroundInputFallback": 0,
    "fakeFocusEnabled": 0,
    "inputStrategy": "auto",
    "cdpPort": 9222
  }

注意：
  · 窗口模式在独立宏桌面执行，用户桌面光标/焦点不受影响
  · 后台窗口模式在用户桌面操作已绑定窗口，不抢焦点
  · 坐标 x/y、searchX1..Y2 在窗口模式下为客户区坐标
  · writeScript / createMacroScript 创建脚本宏时必须写入 windowMode（可为 enabled=0）
  · 默认模式脚本应写入 breakoutTimeSeconds（0 表示禁用）；未写视为 0

【脱离时间 breakoutTimeSeconds — 仅默认模式】

  breakoutTimeSeconds     非负数字，秒。0 或未填写=禁用。
  仅 windowMode.enabled=0 时生效；窗口/后台模式保存时强制为 0。

  行为：宏运行中用户操作鼠标/键盘（移动、点击、滚轮、按键，不含已注册热键）会立即暂停；
  暂停满 breakoutTimeSeconds 秒后从当前动作重试（嵌套宏/指令块内精确到具体步骤）。
  暂停等待期间再有用户操作会重置倒计时。

  createMacroScript / writeScript：
    · scriptMode=default 或未启用 windowMode 时可传 breakoutTimeSeconds
    · 用户要求「允许手动打断后继续」「脱离时间」等场景时设置，常见 1–10 秒
    · 不需要时写 0 或省略

  示例（默认模式，3 秒脱离）：
    "windowMode": { "enabled": 0, ... },
    "breakoutTimeSeconds": 3

)";


const wchar_t* kRefMouseKeyboard = LR"(【鼠标动作】

moveMouse:

  x, y              固定坐标（moveFromVar=0 时）

  randomX, randomY  随机偏移范围

  moveFromVar       1=来自变量表达式

  moveVarExprX/Y    表达式，如 "btn.x+10"、"anchor.x"、"{btnIcon}.y+5"



moveMouseRelative:

  x/dx, y/dy        相对位移像素（可负）；回放用 SendInput 相对移动，适合 FPS 视角

  randomX, randomY  可选随机附加位移（非负）

  录制: 仅当系统光标不可见或 ClipCursor 裁剪时，用 Raw Input 写入；可见光标仍用 moveMouse



mouseClick / mouseDown / mouseUp:

  button            left(默认) / right / middle / x1 / x2

  clickCount        重复次数（mouseClick），默认 1

  duration          两次重复之间的间隔秒数（仅 clickCount>1 生效；执行前/后不等待），默认 0.1

  randomDuration    间隔上的随机附加秒数，默认 0

  holdLeftCtrl 等   修饰键 0/1（mouseDown/Up/Click 可选）



scrollWheel:

  scrollVertical    1=垂直（默认）

  scrollHorizontal  1=水平

  scrollDirection   0=向上/左，1=向下/右

  scrollSteps       步数，默认 1

  clickCount/duration/randomDuration  重复与间隔（仅相邻两次之间等待）


wait:

  duration          等待秒数，默认 0.1

  randomDuration    随机附加秒数



【键盘动作】

keyClick / keyDown / keyUp:

  keyText           按键显示名，如 "A"、"Enter"、"F5"

  keyVk             虚拟键码（单字符可用 ASCII 码）

  holdLeftCtrl 等   修饰键组合

  clickCount/duration/randomDuration  重复与间隔（仅相邻两次之间；count=1 不等待）



hotkeyShortcut:

  shortcutPreset    预设索引 0~8：

    0=Ctrl+C, 1=Ctrl+V, 2=Ctrl+X, 3=Ctrl+S, 4=Ctrl+F,

    5=Alt+F4, 6=Win+D, 7=Win+R, 8=Ctrl+Alt+Delete

  clickCount/duration/randomDuration  同上，间隔仅在两次重复之间



quickInput:

  inputText         要输入的文本（支持 \n \r \t \\ 转义）

  charInterval      字符间隔秒数，默认 0.01

  clickCount/duration/randomDuration  整段输入的重复间隔（非字间；字间用 charInterval）

)";



const wchar_t* kRefFlow = LR"(【流程控制】

loop:

  loopCount         循环次数，-1=无限循环

  loopFromVar       1=次数来自表达式

  loopVarExpr       次数表达式（loopFromVar=1 时）

  loopVarName       循环计数变量名（可选，用于 {变量名} 获取当前第几次）



endLoop:

  必须作为 loop 的子节点（indent = loop.indent + 1），用于提前跳出循环



if:

  conditionExpr     条件表达式（见「条件表达式」章节）

  子动作 indent = if.indent + 1



else:

  与 if 配对，indent 与 if 相同；else 的子动作 indent = else.indent + 1



defineBlock / runBlock:

  blockName         块名（字母开头，仅字母数字）



runMacro / mousePlayback:

  blockName         目标脚本显示名

  targetPath        目标脚本路径

  clickCount/duration/randomDuration  仅 mousePlayback：回放重复次数与两次回放之间的间隔



stopMacro, lockScreenshot, unlockScreenshot:

  无额外必填字段



goto:

  gotoStepExpr      目标动作序号（列表左侧序号，支持变量名或 {变量}）

  跳转范围为当前宏内全部动作（被 runMacro/mousePlayback 调用时以被调宏为准，不会跳到调用方宏）

  可跳出/跳入循环、条件分支与指令块

  跳入循环体内：视为该循环第 1 次迭代，从目标动作开始执行；循环体内互跳仍保持同一次迭代

  跳转到 endLoop 等价于跳出当前最内层循环体



timerRecordTime:

  loopVarName       计时器变量名（引用 {变量名} 得秒数）



getCursorPos:

  matchVarName      变量名（默认 cursor），保存当前屏幕光标坐标

  引用：{变量名}.x / {变量名}.y（与找图变量坐标属性相同）



【系统动作】

openWebpage:  targetPath = URL

openFile:     targetPath = 文件路径

runProgram:   shortcutPreset(0=自选文件) + targetPath + inputText(参数)

closeProgram: targetPath = 进程名/路径；matchFileNameOnly=1 仅匹配文件名

★ 禁止 AI 使用 customText（旧版占位类型）。步骤说明写 remark，勿伪造动作名。

stopMacro（结束宏运行）：
  一次性脚本末尾必须包含；默认回放设置下无 stopMacro 会无限重复执行整份脚本。
  buildScriptActions / createMacroScript 会自动在末尾追加（顶层无限 loop 脚本除外）。

)";



const wchar_t* kRefAi = LR"(【AI 动作 — 必读】

★ 选用优先级（默认效率优先，非 AI 动作优先）：

  1. findImage / textRecognition(OCR) / 常规键鼠 — 首选，能完成就不要用 AI

  2. getCursorPos — 仅需当前光标坐标时

  3. aiTextAnalysis — 必须理解文字语义且 OCR 不够用时（尽量少用）

  4. aiImageAnalysis — 必须理解画面且 findImage 无法胜任时（尽量少用）；
     准确度优先模式下可用于兜底分支诊断当前界面状况

  5. aiActionExecute — ★权重极低★：除非用户明确要求「AI动作执行/让AI自动操作桌面」，
     否则禁止调用 buildAiActionExecuteAction；禁止因任务复杂就用它整段生成脚本



以下四种动作必须使用专用工具构建（禁止用 buildScriptActions 手写）：
  buildGetCursorPosAction / buildAiTextAnalysisAction /
  buildAiImageAnalysisAction / buildAiActionExecuteAction（最后一项见上条限制）
可用 listAiModels 查看已添加模型；图片分析与带截图的执行会自动选择识图模型。



getCursorPos — 获取当前光标位置

  matchVarName      变量名，默认 cursor

  执行后可用 {变量名}.x、{变量名}.y 在后续 moveMouse / if 中引用



aiTextAnalysis — AI 文字分析

  aiPrompt          ★必填：发给模型的分析要求（可含 {变量} 引用）

  aiOutputVarName   输出变量名，默认 aiResult

  aiOutputType      0=文本（默认）  1=数字

  aiModelName       模型名；专用工具会自动从已添加模型中选择（图片分析优先识图模型）

  ★ 优先 OCR(textRecognition)；仅当 OCR 无法表达所需语义时才用本动作



aiImageAnalysis — AI 图片分析

  字段同 aiTextAnalysis，另加：

  aiOutputVarName   默认 aiImgResult

  aiImageScale      截屏缩放 0.1~1.0，默认 1.0

  aiRegionByImage   1=先找图再按相对区域截屏

  aiTargetImagePath 锚定图片路径 images\xxx.bmp

  aiSearchX1/Y1/X2/Y2  截屏区域（不填且 aiRegionByImage=0 时全屏）

  ★ 优先 findImage；准确度兜底时可用来判断界面处于哪种异常状态（见 readAgentSkill section=scriptStrategy）



aiActionExecute — AI 动作执行

  aiPrompt          ★必填：要让 AI 完成的桌面操作任务描述

  aiModelName       模型名；专用工具按 aiWithImage 自动选择

  aiWithImage       1=附带屏幕截图（默认 1）

  aiRegionByImage / aiTargetImagePath / aiSearchX1~Y2  截图区域（同图片分析）

  aiMaxSteps        本动作内 AI 生成步骤上限，默认 10；-1=不限制

  aiConfirmExecute  1=执行前弹窗确认

  aiContextMode / aiTimeoutSec / aiFallbackValue

  ★ 仅当用户明确要求时使用；常规点击/输入/找图必须用 findImage+键鼠 等动作链



★ 典型流程 — 识图后 AI 分析再输入（仅 OCR/找图无法完成时）：

  1. findImage(followUp=2, matchVarName="anchor")

  2. aiImageAnalysis(...)

  3. quickInput(inputText="{aiImgResult}")



★ 典型流程 — 纯文本 AI 结果写入变量后判断：

  1. aiTextAnalysis(...)

  2. if(conditionExpr="summary >> 成功") → 子动作



不确定字段时：readScriptReference(section=ai) 或 readAgentSkill(section=scriptStrategy)

)";



const wchar_t* kRefConditions = LR"(【条件表达式 conditionExpr】

用于 if 动作的 conditionExpr 字段。



运算符：

  ==  !=  <  <=  >  >=  >>（包含，左字符串含右字符串）

逻辑连接：and  or  not（写在子句末尾）



变量写法（以下等价）：

  btnIcon.matchData > 0

  ${btnIcon}.matchData > 0

  {btnIcon.matchData} > 0



常用条件示例：

  btnIcon.matchData > 0                  找图找到了

  btnIcon.matchData >= 65                找图匹配度达标

  btnIcon.matchData > 0 and retryCount < 5

  ocrVar >> 成功                         OCR 文本包含「成功」

  searchBtn == 1                         文字查找找到了

  loopCount >= 10                        循环变量判断



找图变量 + if 完整示例（用户要「找到按钮才点击」）：

  1. findImage: findImageFollowUp=2, matchVarName="btnIcon", imagePath=...

  2. if: conditionExpr="btnIcon.matchData > 0", indent=0

  3. mouseClick 或 moveMouse+mouseClick, indent=1

  4. else, indent=0

  5. wait 或其他, indent=1



★ 不要省略 findImageFollowUp=2；不要只在 remark 里写变量名而不设字段。

)";



const wchar_t* kRefVariables = LR"(【变量系统汇总】

引用语法：${变量名} 或 ${变量名.属性} 或 {变量名.属性}



找图变量（须 findImageFollowUp=2）：

  .matchData .x .y .x1 .y1



OCR 获取文字（ocrResultMode=0）：

  {变量名} → 文本



OCR 文字查找（ocrResultMode=1）：

  {变量名} → 0/1；.x .y .x1 .y1 → 坐标



循环变量（loop 设 loopVarName）：

  {变量名} → 当前循环计数（从 1 开始）



计时器（timerRecordTime 设 loopVarName）：

  {变量名} → 距记录时刻的秒数



光标位置（getCursorPos 设 matchVarName）：

  {变量名}.x / {变量名}.y → 当前屏幕坐标



AI 输出（aiTextAnalysis / aiImageAnalysis）：

  {aiResult} 或自定义 aiOutputVarName → 模型返回的文本/数字

  默认变量名：文字分析 aiResult，图片分析 aiImgResult



内置：

  ${ctrl:CurLoops()} → 宏从头执行的第几次



坐标表达式（moveFromVar=1）：

  moveVarExprX: "btnIcon.x+50"

  moveVarExprY: "btnIcon.y+10"

)";



const wchar_t* kRefPatterns = LR"(【复合模式速查】

1. 找图点击：findImage(followUp=0) — 找到即点击

2. 找图移动：findImage(followUp=1) — 找到即移动鼠标

3. 找图存变量：findImage(followUp=2, matchVarName=自定义名) — ★必须 followUp=2

4. 锚定偏移：followUp=2 存变量 → moveMouse(moveFromVar=1) → mouseClick

5. 找图+条件：followUp=2 → if(matchData>0) → 子动作

6. 锚定 OCR：findImage 存变量 + textRecognition(ocrRegionByImage=1)

7. 文字查找点击：textRecognition(mode=1, followUp=0)

8. 循环：loop(count, indent=0) → 子动作(indent=1) → endLoop(indent=1)

9. 代码块：defineBlock → 子动作 → runBlock（主流程中跳过 defineBlock）

10. 录制优化：硬编码坐标改 findImage；加 wait；加 if 条件

11. AI 动作选用（默认效率优先）：
    · 能用 findImage/OCR/键鼠完成的，禁止用 aiTextAnalysis / aiImageAnalysis
    · aiActionExecute 仅用户明确要求「AI动作执行」时使用
    · 准确度优先时 readAgentSkill section=scriptStrategy 查看兜底模式

12. 准确度兜底 — 关键找图（用户要稳/要兜底时）：
    timerRecordTime → findImage(findTimeExpr=30,followUp=2) →
    if(计时变量<30 and matchData>0) 正常分支 else 兜底分支 →
    兜底内 aiImageAnalysis 判状况 → 按变量选预案 → goto 跳回主流程起点

)";



const wchar_t* kRefMistakes = LR"(【常见 AI 生成错误 — 务必避免】

1. ★ 找图「保存变量」却设 findImageFollowUp=0 或 1

   → 正确：findImageFollowUp=2 + matchVarName="有意义的名字"



2. 只写 matchVarName 不写 findImageFollowUp

   → 默认 followUp=0（点击），变量不会被预留



3. OCR 保存变量却设 ocrFollowUp=0 或 1

   → 正确：ocrFollowUp=2



4. 写了 if 条件但前面没有 followUp=2 的找图/OCR 动作

   → 变量不存在，条件永远为假



5. 找图变量用了 matchRet 但脚本中有多个找图变量

   → 每个找图变量用不同 matchVarName



6. indent 错误：if 的子动作 indent 应为 if.indent+1



7. imagePath 写绝对路径

   → 正确：images\xxx.bmp 相对路径



8. 变量名含中文或特殊字符

   → 正确：英文字母开头，仅字母数字



9. 条件表达式用中文属性名

   → 正确：btnIcon.matchData（不是「匹配度」）



10. 不确定图片路径时在 remark 写「待确认: 请替换 imagePath」



11. ★ 手写 actions JSON 而不调用 buildScriptActions

   → 正确：buildScriptActions 生成后再 writeScript



12. 使用 AI 动作却未写 aiPrompt

   → aiTextAnalysis / aiImageAnalysis / aiActionExecute 均必填 aiPrompt



13. ★ 用 customText 写说明文字当动作

   → 正确：remark 写说明，用 wait/goto/mouseClick 等真实动作



14. ★ 一次性脚本末尾缺少 stopMacro

   → 默认会无限重复执行；工具构建时会自动追加



15. endLoop 不在 loop 子节点内

   → 正确：endLoop 必须是 loop 的子节点（indent = loop.indent + 1），否则会异常结束宏运行



16. ★ 用户未要求「AI动作执行」却使用 aiActionExecute

   → 正确：用 findImage + 键鼠动作链；aiActionExecute 权重极低



17. 能用 findImage / OCR 却用 aiImageAnalysis / aiTextAnalysis

   → 正确：优先常规识别动作；AI 分析仅在语义理解必需或准确度兜底诊断时使用



18. 准确度优先却未对关键找图做兜底

   → 正确：timerRecordTime + findTimeExpr 限时找图 + if 分支 + 兜底后 goto 回主流程起点

)";



const wchar_t* kRefActions = LR"(【动作类型索引】

基础：wait, moveMouse, moveMouseRelative, mouseClick, mouseDown, mouseUp,

      keyClick, keyDown, keyUp, quickInput, hotkeyShortcut,

      scrollWheel

流程：loop, endLoop, defineBlock, runBlock, if, else, stopMacro, goto

识别：findImage, textRecognition

系统：openWebpage, openFile, runProgram, closeProgram,

      timerRecordTime, getCursorPos, lockScreenshot, unlockScreenshot

AI：aiTextAnalysis, aiImageAnalysis, aiActionExecute

回放：runMacro, mousePlayback



详细字段请查阅对应 section：

  findImage → section=findImage

  textRecognition → section=ocr

  AI 动作 → section=ai

  鼠标键盘 → section=mouse

  流程 → section=flow

  条件 → section=conditions

  变量 → section=variables

  错误清单 → section=mistakes

)";



std::wstring BuildFullReference() {

    return std::wstring(L"【脚本助手技术参考 — 完整版】\n")

        + kRefFormat + kRefActions + kRefFindImage + kRefTextRecognition + kRefWindowMode

        + kRefMouseKeyboard + kRefFlow + kRefAi + kRefConditions + kRefVariables

        + kRefPatterns + kRefMistakes;

}



}  // namespace



std::wstring AgentReferenceGet(const std::wstring& section) {

    const std::wstring key = Trim(section);

    if (key.empty() || key == L"all" || key == L"full" || key == L"全部")

        return BuildFullReference();

    if (key == L"format" || key == L"格式")

        return kRefFormat;

    if (key == L"actions" || key == L"动作")

        return kRefActions;

    if (key == L"findImage" || key == L"找图")

        return kRefFindImage;

    if (key == L"ocr" || key == L"textRecognition" || key == L"文字识别")

        return kRefTextRecognition;

    if (key == L"windowMode" || key == L"windowmode" || key == L"窗口模式" || key == L"脚本模式")

        return kRefWindowMode;

    if (key == L"breakoutTime" || key == L"breakoutTimeSeconds" || key == L"脱离时间")

        return kRefWindowMode;

    if (key == L"mouse" || key == L"keyboard" || key == L"鼠标" || key == L"键盘")

        return kRefMouseKeyboard;

    if (key == L"flow" || key == L"流程")

        return kRefFlow;

    if (key == L"ai" || key == L"AI" || key == L"人工智能")

        return kRefAi;

    if (key == L"conditions" || key == L"condition" || key == L"条件")

        return kRefConditions;

    if (key == L"variables" || key == L"变量")

        return kRefVariables;

    if (key == L"patterns" || key == L"模式")

        return kRefPatterns;

    if (key == L"mistakes" || key == L"errors" || key == L"错误")

        return kRefMistakes;

    return BuildFullReference()

        + L"\n可用 section: all, format, actions, findImage, ocr, windowMode, breakoutTime, mouse, flow, ai,"

        L" conditions, variables, patterns, mistakes";

}



AgentTool MakeReadScriptReferenceTool() {

    AgentTool tool;

    tool.name = L"readScriptReference";

    tool.description =

        L"读取脚本格式 Skill。section: format|findImage|ocr|windowMode|breakoutTime|flow|ai|conditions|variables|patterns|mistakes|all。"
        L"动作 JSON 用 buildScriptActions 或 buildAi* 工具生成，勿手写。"
        L"优化/定时任务/设置/回复风格用 readAgentSkill。";

    tool.parameters_json = LR"({

        "type": "object",

        "properties": {

            "section": {

                "type": "string",

                "description": "all | format | actions | findImage | ocr | windowMode | breakoutTime | mouse | flow | ai | conditions | variables | patterns | mistakes"

            }

        },

        "required": []

    })";

    tool.execute = [](const std::wstring& paramsJson) -> std::wstring {

        std::wstring section;

        try {

            const json p = json::parse(ToUtf8(paramsJson));

            if (p.contains("section") && p["section"].is_string())

                section = FromUtf8(p["section"].get<std::string>());

        } catch (...) {}

        return AgentReferenceGet(section);

    };

    return tool;

}

const wchar_t* kSkillScriptStrategy = LR"(【脚本生成策略 — readAgentSkill section=scriptStrategy】

用户未说明时默认「效率优先」。用户说「准确度优先/要稳/要可靠/要兜底」时切换为准确度模式。



── 效率优先（默认）──

· 动作链：findImage、OCR、wait、键鼠、if、goto，尽量不用 AI 文字/图片分析

· 禁止 aiActionExecute，除非用户明确要求「AI动作执行/让AI自动操作」

· 找图：followUp=2 存变量 + if(matchData>0) 即可，不必加计时器兜底

· 验证步骤是否成功：用 findImage/OCR 再次识别，不用 AI 分析

· 脚本头字段：createMacroScript 须写 windowMode（默认 enabled=0）；默认模式 breakoutTimeSeconds 未写视为 0
  用户要求「手动操作后暂停再继续/脱离时间」时设 breakoutTimeSeconds（常见 1–10 秒）



── 准确度优先 ──

· 仍禁止 aiActionExecute（即使用户要稳，也用常规动作+兜底，不用 AI 代操作）

· 可适度使用 aiImageAnalysis 在兜底分支诊断界面；aiTextAnalysis 仅在 OCR 不够时

· 关键步骤（尤其找图）须考虑失败并写兜底，但兜底只做环境恢复，不重写整份业务逻辑



★ 关键找图兜底模板（准确度模式，找图前必加计时器）：

  假设主流程从第 MAIN 步开始（页面加载完成后的第一个业务动作序号）。

  1. timerRecordTime(loopVarName="findTimer")     remark: 开始计时

  2. findImage(findImageFollowUp=2, matchVarName="target",
     findTimeExpr="30", imagePath=...)             remark: 限时30秒找图

  3. if(conditionExpr="findTimer < 30 and target.matchData > 0", indent=0)

  4.   → 正常后续动作（indent=1）

  5. else (indent=0)

  6.   → 兜底分支（indent=1）：

       a. aiImageAnalysis(
            aiPrompt="分析当前屏幕。仅回复一个数字，不要其它文字："
                     "1=仍在加载请等待 2=弹窗遮挡需关闭 3=页面错误需刷新重进",
            aiOutputVarName="fallbackPlan")

       b. if(fallbackPlan == 1) → wait 5秒 等加载

       c. else if(fallbackPlan == 2) → 找关闭按钮并点击

       d. else if(fallbackPlan == 3) → 刷新/重新打开界面（按实际场景写具体动作）

       e. goto(gotoStepExpr="MAIN")  ★兜底完成后跳回主流程起点，勿重复后面全部步骤

  findTimeExpr 秒数与 findTimer 判断阈值一致（如均为 30）。

  正常分支条件须同时检查 matchData>0，避免误匹配仍走正常逻辑。



── 模式识别 ──

· 「快点/效率/简单」→ 效率优先

· 「稳/可靠/容错/兜底/别失败」→ 准确度优先，读本节并套用兜底模板

· 「用AI执行/AI自动操作」→ 才允许 buildAiActionExecuteAction

)";

const wchar_t* kSkillReply = LR"(【回复风格 — readAgentSkill section=reply】

思考与回复都会展示给用户。

规则：
1. 口语化中文纯文本，像聊天一样自然。
2. 禁止 Markdown：不要用井号标题、星号粗体、反引号、表格、短横线列表。
3. 禁止在思考或回复中出现任何英文字段名/工具名（如 enableCoordinateJitter、listScripts、keyClick、goto、endLoop）。
   工具参数里的英文只是后台标识，对用户说中文含义即可。
4. 说明脚本动作时，必须使用编辑器动作列的中文名称（与 readScript 返回的「动作一览」一致），
   例如「第9步 跳出循环」「按键点击」「跳转」「结束宏运行」，禁止说英文 type。
5. 分点用 1. 2. 3. 或 · 开头。

常见翻译（心里对照，不要说英文）：
  坐标抖动、回放次数、随机间隔、定时任务、鼠标宏、键鼠录制、识图、图片分析
)";

std::wstring BuildReplySkillText() {
    return std::wstring(kSkillReply) + L"\n\n" + ActionTypeReplyCatalog();
}

const wchar_t* kSkillOptimize = LR"(【脚本优化 — readAgentSkill section=optimize】

用户要求优化时，用 optimizeScript 或 optimizeRecording，不要手动读写模拟。

merge 模式：合并任意两个关键操作之间的移动+等待 → 一次等待+一次移动。
  等待默认累加（保持总时长）；用户可指定 average/first/last。

compressPath 模式：按 distanceThreshold 去掉过密移动点，compressWait 为间隔。

可另存为其他文件名保留原版。
)";

const wchar_t* kSkillScheduledTasks = LR"(【定时任务 — readAgentSkill section=scheduledTasks】

工具：listScheduledTasks / createScheduledTask / updateScheduledTask / deleteScheduledTask

流程：
1. 确认目标脚本与时间
2. 无脚本则先 createMacroScript，再创建任务
3. createScheduledTask 必须带 targetFile（真实存在的脚本/录制文件名）

频率：
  custom=单次（必填 year/month/day + 时分秒）
  daily=每天（时分）
  weekly=每周（时分 + weekDays，至少一个；「每周天」=周日）
  hourly=每小时（分）

默认创建鼠标宏类型任务，除非用户明确说是录制。
创建/更新/删除后主窗口会 Reload，无需用户再开一次定时对话框。
)";

const wchar_t* kSkillSettings = LR"(【应用设置 — readAgentSkill section=settings】

工具：listSettings / updateSettings（不可改 AI 助手自身设置）

分类：click=点击, playback=回放, other=其他

常见意图：
  「无限循环太烦」→ 回放次数限制开，设 1 次
  「宏后不要隐藏窗口」→ 关自动隐藏主窗口
  「关随机间隔」→ 关点击随机间隔
  「回放间隔 N 秒」→ 开回放间隔，最小最大都设 N

生效：回放类下轮循环生效；其他下次启动宏生效。
)";

std::wstring AgentSkillGet(const std::wstring& section) {
    std::wstring key = Trim(section);
    for (auto& c : key) c = static_cast<wchar_t>(std::towlower(c));

    if (key.empty() || key == L"all") {
        return BuildReplySkillText() + L"\n\n" + kSkillScriptStrategy + L"\n\n" + kSkillOptimize + L"\n\n"
            + kSkillScheduledTasks + L"\n\n" + kSkillSettings
            + L"\n\n可用 section: all, reply, scriptStrategy, optimize, scheduledTasks, settings";
    }
    if (key == L"reply" || key == L"style" || key == L"回复")
        return BuildReplySkillText();
    if (key == L"scriptstrategy" || key == L"strategy" || key == L"脚本策略" || key == L"策略")
        return kSkillScriptStrategy;
    if (key == L"optimize" || key == L"optimization" || key == L"优化")
        return kSkillOptimize;
    if (key == L"scheduledtasks" || key == L"scheduled" || key == L"tasks" || key == L"定时")
        return kSkillScheduledTasks;
    if (key == L"settings" || key == L"setting" || key == L"设置")
        return kSkillSettings;

    return AgentSkillGet(L"all");
}

AgentTool MakeReadAgentSkillTool() {
    AgentTool tool;
    tool.name = L"readAgentSkill";
    tool.description =
        L"读取助手操作 Skill（非脚本格式）。"
        L"section: reply|scriptStrategy|optimize|scheduledTasks|settings|all。"
        L"生成/修改脚本涉及 AI 动作选用、效率/准确度、兜底逻辑时先读 scriptStrategy；"
        L"涉及优化、定时任务、改设置、回复风格时读对应 section。";
    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "section": {
                "type": "string",
                "description": "reply | scriptStrategy | optimize | scheduledTasks | settings | all"
            }
        },
        "required": []
    })";
    tool.execute = [](const std::wstring& paramsJson) -> std::wstring {
        std::wstring section;
        try {
            const json p = json::parse(ToUtf8(paramsJson));
            if (p.contains("section") && p["section"].is_string())
                section = FromUtf8(p["section"].get<std::string>());
        } catch (...) {}
        return AgentSkillGet(section);
    };
    return tool;
}

