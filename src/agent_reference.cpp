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
  recordingCaptureMode  -1/缺失=旧文件，0=自动，1=绝对坐标，2=相对坐标，3=图片定位
  inputTimingVersion    0/缺省=旧文件；1=微秒轴但前延迟可挂在动作上；2=时间只在显式 wait（及重复间隔）

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

  indent      缩进层级：0=顶层；if/loop/defineBlock/watchImage 的子动作 = 父级 indent+1

  no / text   由 buildScriptActions / createMacroScript 自动生成，禁止手写



缩进规则：

  loop / if / else / defineBlock / watchImage 是容器，其内部动作 indent 比容器大 1。

  推荐用 children 嵌套子动作（像写代码），buildScriptActions 会展开为 indent。

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

  2 = 保存匹配度 — 只把匹配结果写入变量，不点击也不移动（原「保存到变量」）

  3 = 保存图片 — 截取区域（或锚点+相对区）保存到路径或临时图片变量



★ 常见错误：用户要求「预留/保存匹配度变量」时，必须设 findImageFollowUp=2。

  仅写 matchVarName 而不设 findImageFollowUp=2，软件会当作「点击」执行，变量不会被正确预留。

  用户要求「保存截图/图片变量」时，必须设 findImageFollowUp=3（不是 2）。



专用字段：

  imagePath         模板路径 images\xxx.bmp；imageUseVar=1 时为变量名或绝对路径

  imageUseVar       1=「要查找的图」走变量/路径输入（不强制库内模板文件）

  matchThreshold    匹配阈值 1~100，默认 65；perfectMatch=1 时忽略

  perfectMatch      1=完美匹配（粗定位+邻域精修+每通道≤1 像素终审；通过 score=100）

  searchFullScreen  1=全屏搜索，0=区域搜索（此时需 searchX1/Y1/X2/Y2）。
                    窗口/后台窗口模式忽略此项，始终在整个目标窗口内搜索。

  searchX1/Y1/X2/Y2 搜索区域坐标（searchFullScreen=0 时有效）；followUp=3 无模板时为截图区域。
                    窗口/后台窗口模式忽略绝对选取区；根据图片选取区域用 imageRegionX1~Y2。

  imageScaleMin     最小缩放，默认 1.0

  imageScaleMax     最大缩放，默认 1.0

  imageScale        缩放中值，通常 (min+max)/2

  findImageFollowUp 后续操作：0点击 / 1移动 / 2保存匹配度 / 3保存图片

  offsetX, offsetY  点击/移动时的偏移（followUp=0或1 时有效）

  findTimeExpr      找图时限（秒，可小数）。0=只找一次；负数（如 -1）=直到找到；可填变量名。
                    点击/移动/保存匹配度，以及「保存图片且有模板」都生效；保存图片无模板（纯截屏）忽略。

  findUntilFound    OCR 文字查找：1=循环直到找到（找图动作请用 findTimeExpr）

  matchVarName      followUp=2 默认 matchRet；followUp=3 默认 image（变量名或持久路径）

  imageRegionX1~Y2  followUp=3 有模板时：匹配框内相对截图区



变量名规则：英文字母开头，仅含字母和数字，如 btnIcon、loginBtn、matchRet、image。

  followUp=3 的 matchVarName 若含 \\ / 或盘符则视为持久路径（覆盖写盘）。



保存匹配度（followUp=2）后可用属性（在条件/坐标表达式中引用）：

  变量名.matchData  匹配度（整数 0~100；未找到或未达阈值为 0）

  变量名.x          匹配区域左上角 X

  变量名.y          匹配区域左上角 Y

  变量名.x1         匹配区域右下角 X

  变量名.y1         匹配区域右下角 Y



保存图片（followUp=3）临时变量：{变量名} → 运行期临时 BMP 绝对路径；宏结束自动删除。



★ 标准模板 — 找图保存匹配度 + 条件判断：

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

★ 找图监视是独立顶层容器 type=watchImage（主流程跳过；参数见 section=flow 或 lookupMacroAction type=watchImage），不要改成 findImage。

)";

const wchar_t* kRefMultiMatch = LR"(【多图匹配 multiMatch】
type 必须为 "multiMatch"。独立叶子动作，不要改成 findImage，也不要用 children。
imagePaths[] 必填（最多 8 张）；imagePath 镜像首张。每张可 imageUseVar / imageUseVars[]（与找图相同的变量图）。
multiMatchMode：0=多图择一（按列表顺序，第一张过阈值即停）；1=一图多处（只用第一张，NMS 多匹配，最多 multiMatchMax 处，默认 20）。
切模式不删图。第一版不做「每张图都找齐所有点」。
followUp：0 点击（一图多处=依次点击，间隔 duration 默认 0.05）；1 移动到第一处；2 保存匹配度。无保存图片。
findTimeExpr 与找图一样：等到至少一处；等待中会触发找图监视。
变量：{matchRet[0]} 第一处是否命中；{matchRet[0].x} 左上角 X（还有 .y/.x1/.y1/.cx/.cy/.matchData/.hit/.hitName）；{matchRet.count} 命中个数。
{matchRet[n]} 是下拉代指，运行时字面 n 不解析。
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

  ocrRegionByImage  1=根据找图锚点确定 OCR 区域（需 imagePath + imageRegionX1~Y2）

  ocrDigitsOnly     1=纯数字（小框整行识别；全图缩小检测 + 英文数字识别头，跳过方向分类）

  searchFullScreen  找图/OCR 搜索区是否全屏（绝对坐标）

  searchX1/Y1/X2/Y2 找图搜索区（屏幕绝对坐标；锚定模式下在此区内找图）

  imageRegionX1~Y2  匹配框内相对 OCR 区域（「选取区域位置」写入；不填则整框）

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
  windowMode.selectMethod         selectOnStartup | useEditorWindowClass | noSelect
                                  （旧脚本 mousePositionOnStartup 读入后等同 selectOnStartup：启动时取当前聚焦窗）
  windowMode.targetPickX/Y        准星绑定坐标
  windowMode.coordSpace           windowClient（默认）| screenAbsolute
  windowMode.windowRelativeCoordinates  1=动作坐标已是目标客户区（窗口模式录制）
  windowMode.recordClientWidth/Height   录制时目标客户区像素；回放按当前窗口大小缩放。旧脚本可缺省（则用 coordMeta.capture）
  windowMode.autoLaunchTarget     1=运行前自动启动目标程序
  windowMode.launchArgs           启动参数
  windowMode.allowForegroundInputFallback  后台输入失败时是否抢焦点（默认 0）
  windowMode.fakeFocusEnabled              游戏假焦点（进程注入 GetForegroundWindow/光标/按键）；宏桌面与后台窗口均可。默认 0，但 Unity/Unreal 等类名会自动启用。勿用于联机/反作弊（英雄联盟等会拒绝访问并无法后台键鼠）；CDP 策略下忽略
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
  · 坐标 x/y 在窗口模式下为客户区坐标；searchX1..Y2 在窗口/后台窗口模式忽略（整窗搜索）
  · 「根据图片选取区域」在窗口内命中后再按 imageRegion 二次筛选
  · writeScript / createMacroScript 创建脚本宏时必须写入 windowMode（可为 enabled=0）
  · 默认模式脚本应写入 breakoutTimeSeconds（0 表示禁用）；未写视为 0

【脱离时间 breakoutTimeSeconds — 仅默认模式】

  breakoutTimeSeconds     非负数字，秒。0 或未填写=禁用。
  仅 windowMode.enabled=0 时生效；窗口/后台模式保存时强制为 0。

  行为：宏运行中用户操作鼠标/键盘（移动、点击、滚轮、按键，不含已注册热键）会立即暂停；
  按住键或鼠标按钮期间视为持续交互，不开始脱离倒计时；松开后且空闲满
  breakoutTimeSeconds 秒才从当前动作重试（嵌套宏/指令块内精确到具体步骤）。
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

  ★ 录制/精密轨迹：间隔只用 type=wait（可用 timingUs）；moveMouse/mouseDown/mouseUp/keyDown/keyUp 不要写 duration 当延时（瞬时动作 duration=0）



moveMouseRelative:

  x/dx, y/dy        相对位移像素（可负）；回放用 SendInput 相对移动，适合 FPS 视角

  randomX, randomY  可选随机附加位移（非负）

  录制: 仅当系统光标不可见或 ClipCursor 裁剪时，用 Raw Input 写入；可见光标仍用 moveMouse



mouseClick / mouseDown / mouseUp:

  button            left(默认) / right / middle / x1 / x2

  clickCount        重复次数（mouseClick），默认 1

  duration          mouseClick：两次重复之间的间隔（仅 clickCount>1）；mouseDown/Up：应为 0（勿当前延迟）

  randomDuration    间隔上的随机附加秒数，默认 0

  holdLeftCtrl 等   修饰键 0/1（mouseDown/Up/Click 可选）



mouseDrag:

  button            left(默认) / right / middle / x1 / x2

  x, y              起点（绝对屏幕坐标；找图定位时为相对图中心偏移，可负）

  endX, endY        终点（同上）

  randomX/Y / randomEndX/Y  起点/终点随机像素

  duration          拖拽时长（按下到松开），默认 0.3；不是点击的重复间隔

  randomDuration    拖拽时长上的随机附加秒数

  holdLeftCtrl 等   修饰键 0/1

  imageLocate       1=找图定位：先找图，起点/终点相对图中心；未找到则跳过本步

  找图定位时还需 imagePath、searchFullScreen/searchX1~Y2、matchThreshold、findTimeExpr、缩放



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

  shortcutPreset    预设索引 0~11：

    0=Ctrl+C, 1=Ctrl+V, 2=Ctrl+X, 3=Ctrl+S, 4=Ctrl+F,

    5=Alt+F4, 6=Win+D, 7=Win+R, 8=Ctrl+Alt+Delete,

    9=Alt+Tab, 10=Win+Down, 11=Win+Up

  clickCount/duration/randomDuration  同上，间隔仅在两次重复之间



quickInput:

  inputText         要输入的文本
  parseEscapes      1=解析文本和变量中的 \n \r \t \\；缺省 0=按字面（变量里的换行/Tab 丢掉）
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

  clickCount/duration/randomDuration  仅 runBlock：整段块重复次数与两遍之间的间隔



watchImage:

  imagePath         监视用图（主流程跳过本容器）

  resumeAfterWatch  1=中断后从原处重跑该次找图；0=跳到本监视容器后的主流程

  watchMode         0/action=动作监视（任意找图等待时顺带搜索）；1/time=时间监视（按间隔轮询）

  watchPollSeconds  时间监视间隔秒，默认 1，最小 0.05

  children[]        命中后执行。子树里若执行了跳转（pending goto）则以跳转为准



varCompute:

  computeCode       类 C 源码（int/double/string、if/else、for/while）

  行末分号可省略（换行即新语句）

  字符串用 "+" 或 '+'（裸写 + 是加法，不能拿来和 OCR 符号比较）

  split(s, "/") 按分隔符拆成数组；parts[0] / parts[-1] / parts.count（或 .length）

  split(s, "/", 2) 最多 2 段，最后一段保留剩余内容；split(s, "") 按字符拆

  toInt("123") / toString(x) / trim(s)

  局部变量默认销毁；末尾 return a, b 导出给后续动作

  ctrl:Clipboard()  为剪贴板文本或文件路径字符串（不是条件里的 0/1）



runMacro / mousePlayback:

  blockName         目标脚本显示名（mousePlayback 界面名=运行录制回放）

  targetPath        目标脚本路径

  clickCount/duration/randomDuration  整段重复次数与两遍之间的间隔（count=1 不等待）
  playbackSpeed     仅 mousePlayback：嵌套录制回放倍速 0.25~4，缺省 1；不叠加设置全局倍速
  useMode           0默认 / 1窗口 / 2后台窗口 / 3继承（缺省 3，跟当前主宏模式走）
  breakoutTimeSeconds  仅默认模式脱离时间（秒，0=禁用）
  nestedWindowMode  窗口/后台窗口绑窗配置（selectMethod/targetExePath/windowClassName 等）



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

  matchVarName      变量名（默认 a），保存当前屏幕光标坐标

  引用：{变量名}.x / {变量名}.y（与找图变量坐标属性相同）



【系统动作】

openWebpage:  targetPath = URL

openFile:     targetPath = 文件路径

activateWindow: targetPath = 窗口标题/进程名子串（宏回放切前台）

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

  matchVarName      变量名，默认 a

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

  aiRegionByImage   1=在绝对识别区域内找图，再按 imageRegion 收窄截屏区

  aiTargetImagePath 锚定图片路径 images\xxx.bmp

  aiSearchX1/Y1/X2/Y2  识别区域（屏幕绝对坐标；不填则全屏）

  imageRegionX1~Y2  匹配框内相对截屏区（不填则整框）

  ★ 优先 findImage；准确度兜底时可用来判断界面处于哪种异常状态（见 readAgentSkill section=scriptStrategy）



aiActionExecute — AI 动作执行

  aiPrompt          ★必填：要让 AI 完成的桌面操作任务描述

  aiModelName       模型名；专用工具按 aiWithImage 自动选择

  aiWithImage       1=附带屏幕截图（默认 1）

  aiLogicConvert    1=逻辑转化（段末固化指令块+找图门闩+else 回退 AI）；默认 0
                    ★仅用户明确要求「逻辑转化/自愈脚本」时置 1

  aiLogicBlockName  关联 DefineBlock 名（可空自动生成）

  aiRegionByImage / aiTargetImagePath / aiSearchX1~Y2  截图区域（同图片分析）

  aiMaxSteps        本动作内 AI 生成步骤上限，默认 10；-1=不限制

  aiContextMode / aiTimeoutSec / aiFallbackValue

  ★ 仅当用户明确要求时使用；常规点击/输入/找图必须用 findImage+键鼠 等动作链
  ★ 逻辑转化脚手架：一个 RunBlock 入口 + 薄/空快路径 + else 中带逻辑转化的 AI；
    禁止整脚本只有一个全能 AI 动作执行



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



图片变量（须 findImageFollowUp=3，matchVarName 为变量名而非路径）：

  {变量名} → 临时图片绝对路径（宏结束释放）

  消费：后续 findImage/OCR/AI 勾选 imageUseVar（或 aiImageUseVar）并填该变量名



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



变量运算（varCompute 的 return a, b）：

  {a} / {b} → 导出的脚本变量（未 return 的局部名不可用）

  运算源码里写裸名 a，不要写成 {a}



内置：

  ${ctrl:CurLoops()} → 宏从头执行的第几次
  ${ctrl:Random()} → 随机变量：每次引用随机取 1~100 的整数
    （可配合 if/else 做随机分支，如 ctrl:Random() > 50）
  ${ctrl:Hour()} / ${ctrl:Minute()} → 本地时当前小时 0–23 / 分钟 0–59
  ${ctrl:Clipboard()} → 剪贴板：条件里非空为1；输入展开文本或文件路径；AI 可附图
  ${Now} / ${time:格式} / ${date:格式} / ${clipboard} / ${random:1,100}
  ${cursor.x} / ${cursor.y} / ${screen.w} / ${screen.h} / ${username}



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

8. 循环：loop(children=[子动作, …])；或 indent=0 的 loop + indent=1 的子动作。禁止循环体与 loop 同级。

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

基础：wait, moveMouse, moveMouseRelative, mouseClick, mouseDrag, mouseDown, mouseUp,

      keyClick, keyDown, keyUp, quickInput, hotkeyShortcut,

      scrollWheel

流程：loop, endLoop, defineBlock, runBlock, if, else, stopMacro, goto, varCompute

识别：findImage, multiMatch（多图择一 / 一图多处）, watchImage（watchMode=动作监视/时间监视 + watchPollSeconds）, textRecognition,
      getColor, findColor, colorMatch（均可 imageLocate=找图定位）

系统：openWebpage, openFile, runProgram, closeProgram,

      timerRecordTime, getCursorPos, lockScreenshot, unlockScreenshot

AI：aiTextAnalysis, aiImageAnalysis, aiActionExecute

回放：runMacro, mousePlayback（界面名=运行录制回放）



详细字段请查阅对应 section：

  findImage → section=findImage

  textRecognition → section=ocr

  AI 动作 → section=ai

  鼠标键盘 → section=mouse

  系统动作（openFile/runProgram/activateWindow 等）→ section=system

  流程 → section=flow

  条件 → section=conditions

  变量 → section=variables

  错误清单 → section=mistakes

)";



const wchar_t* kRefSystem = LR"(【系统动作 — 打开/运行/关闭/激活/回放/计时】

openFile（用系统默认程序打开文件）:
  targetPath      文件路径，如 C:\Users\Public\Desktop\test.txt
  ★ 路径不存在时程序可能弹错或提示选择打开方式；不确定时先确认路径再生成

openWebpage（用默认浏览器打开网页）:
  targetPath      URL，如 https://example.com

runProgram（运行程序/命令）:
  targetPath      程序路径或可执行文件名，如 notepad.exe、powershell.exe
  inputText       命令行参数（可选）
  shortcutPreset  快捷预设：0=自选(targetPath) 1=记事本 2=计算器 3=画图 4=文件管理器
                  5=命令行 6=PowerShell 7=进程管理器 8=注册表编辑器 9=服务
                  10=计算机管理 11=控制面板 12=设置；传 targetPath 时用 0

closeProgram（关闭程序/窗口）:
  targetPath        程序名或窗口标题
  matchFileNameOnly 1=只按文件名匹配 0=按窗口标题匹配（默认）

activateWindow（激活/聚焦窗口）:
  targetPath      窗口标题（也可用 match 字段，二者同义）

runMacro / mousePlayback（运行另一个脚本/录制；mousePlayback 界面名=运行录制回放）:
  targetPath      目标文件名，如 mymacro.json
  blockName       显示名（可选）
  clickCount/duration/randomDuration  整段重复与两遍之间的间隔（count=1 不等待）
  playbackSpeed   仅 mousePlayback：0.25~4 缺省 1；嵌套录制只用此字段，不叠加设置全局倍速
  useMode         0默认/1窗口/2后台窗口/3继承（缺省3=跟主宏走）
  breakoutTimeSeconds 仅默认模式脱离秒数
  nestedWindowMode 窗口/后台窗口绑窗对象（同脚本级 windowMode 字段）

runBlock（运行宏指令块）:
  blockName       块名
  clickCount/duration/randomDuration  同上，整段块重复

timerRecordTime（计时，秒）:
  loopVarName     时间变量名，如 findTimer；之后用 {findTimer} 读取已计时秒数

getCursorPos（读取光标坐标）:
  matchVarName    变量名（默认 a）；之后用 {a.x} {a.y} 引用

lockScreenshot / unlockScreenshot:
  无参数；锁定/解锁截图画面（配合找图/OCR 使用）

stopMacro:
  结束宏运行；buildScriptActions 会自动在非无限循环脚本末尾追加，不要手写

【路径与等待要点】

· 用户桌面是 %USERPROFILE%\Desktop（如 C:\Users\当前用户名\Desktop），
  不是 C:\Users\Public\Desktop（公共桌面）。
  路径不确定（尤其涉及用户名）时，先用 runAgentCommand 查询真实路径
  （如 echo %USERPROFILE%\Desktop），再填 targetPath。
· 打开文件 / 运行程序 / 打开网页 / 双击打开 之后，必须等窗口或程序起来再继续：
  后续 wait 建议 3 秒以上（冷启动更久，可用 3~5 秒），不要用 0.5~1.5 秒短等待。
)";

std::wstring BuildFullReference() {

    return std::wstring(L"【脚本助手技术参考 — 完整版】\n")

        + kRefFormat + kRefActions + kRefFindImage + kRefMultiMatch + kRefTextRecognition + kRefWindowMode

        + kRefMouseKeyboard + kRefFlow + kRefAi + kRefConditions + kRefVariables

        + kRefPatterns + kRefMistakes + kRefSystem;

}



}  // namespace



std::wstring AgentReferenceGet(const std::wstring& section) {

    const std::wstring key = Trim(section);

    if (key.empty() || key == L"catalog" || key == L"index" || key == L"list")

        return std::wstring(L"## Script Reference 目录（请指定 section，勿一次 all）\n")
            + L"- format / actions / findImage / multiMatch / ocr / windowMode / breakoutTime\n"
            + L"- mouse / system / flow / ai / conditions / variables / patterns / mistakes\n"
            + L"调用：readScriptReference({section:\"findImage\"})";

    if (key == L"all" || key == L"full" || key == L"全部")

        return BuildFullReference();

    if (key == L"format" || key == L"格式")

        return kRefFormat;

    if (key == L"actions" || key == L"动作")

        return kRefActions;

    if (key == L"system" || key == L"系统"
        || key == L"openFile" || key == L"runProgram"
        || key == L"openWebpage" || key == L"closeProgram"
        || key == L"activateWindow" || key == L"timerRecordTime"
        || key == L"getCursorPos" || key == L"runMacro"
        || key == L"mousePlayback")

        return kRefSystem;

    if (key == L"findImage" || key == L"找图")

        return kRefFindImage;

    if (key == L"multiMatch" || key == L"多图匹配" || key == L"multimatch")

        return kRefMultiMatch;

    if (key == L"ocr" || key == L"textRecognition" || key == L"文字识别")

        return kRefTextRecognition;

    if (key == L"windowMode" || key == L"windowmode" || key == L"窗口模式" || key == L"脚本模式")

        return kRefWindowMode;

    if (key == L"breakoutTime" || key == L"breakoutTimeSeconds" || key == L"脱离时间")

        return kRefWindowMode;

    if (key == L"mouse" || key == L"keyboard" || key == L"鼠标" || key == L"键盘")

        return kRefMouseKeyboard;

    if (key == L"flow" || key == L"流程"
        || key == L"watchImage" || key == L"找图监视"
        || key == L"varCompute" || key == L"变量运算")

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

    return std::wstring(L"[未知 section] ") + section
        + L"\n\n可用：format, actions, findImage, ocr, windowMode, breakoutTime, mouse, system,"
        L" flow, ai, conditions, variables, patterns, mistakes（勿一次 all）";

}



AgentTool MakeReadScriptReferenceTool() {

    AgentTool tool;

    tool.name = L"readScriptReference";

    tool.description =

        L"读取脚本格式参考。section 必填：format|findImage|ocr|windowMode|breakoutTime|flow|ai|…"
        L"省略只返回目录。openFile/runProgram/activateWindow 等系统动作参数在 system。"
        L"禁止无必要传 all。动作 JSON 用 buildScriptActions / buildAi* 生成。";

    tool.parameters_json = LR"({

        "type": "object",

        "properties": {

            "section": {

                "type": "string",

                "description": "all | format | actions | findImage | ocr | windowMode | breakoutTime | mouse | system | flow | ai | conditions | variables | patterns | mistakes"

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

【脚本生成规范（铁律，生成/修改脚本前必读）】

1. 必须用工具生成，杜绝手写 JSON：含循环/条件时先 planScriptActions 核对动作树
   （children 嵌套），再 buildScriptActions 校验（自动补序号、标准字段、末尾 stopMacro），
   再用 createMacroScript 保存到 scripts（默认不分类，用户指明目录时传 folder）；
   禁止直接在 writeScript 或回复里手写动作 JSON。
2. 必填参数（缺了会构建失败）：keyClick/keyDown/keyUp→keyText；quickInput→inputText；
  wait→duration；findImage/watchImage→imagePath；textRecognition→imagePath 或 ocrSearchText；
   if→conditionExpr；goto→gotoStepExpr；defineBlock/runBlock→blockName；
   varCompute→computeCode；runMacro/mousePlayback→targetPath；openFile/runProgram/openWebpage/closeProgram/
   activateWindow→targetPath；AI 动作→aiPrompt。
   runMacro/runBlock/mousePlayback 可选 clickCount/duration/randomDuration（整段重复；
   间隔为两遍之间；count=1 不等待）。mousePlayback 另可选 playbackSpeed（0.25~4，缺省 1；
   嵌套录制只用此字段，不叠加设置全局倍速）。runMacro/mousePlayback 另可选 useMode
   （0默认/1窗口/2后台窗口/3继承，缺省3=跟当前主宏模式走）、breakoutTimeSeconds（仅默认模式脱离）、
   nestedWindowMode（窗口/后台窗口绑窗，字段同脚本 windowMode）。mousePlayback 界面名=运行录制回放。
   watchImage 可选 watchMode（0/action=动作监视，找图等待时顺带搜；1/time=时间监视）
   与 watchPollSeconds（时间监视间隔秒，默认 1）。
3. 支持魔法变量（引擎运行时自动求值，inputText/aiPrompt/条件/goto 均可用）：
   - 剪贴板：界面下拉只有 {ctrl:Clipboard()}（条件非空为1，输入展开文本或路径，AI可附图）；
     变量运算里 ctrl:Clipboard() 是文本或文件路径字符串，不是 0/1；
     字符串用 "+" 或 '+'（裸写 + 是加法，不能和 OCR 识别的加减号比较）；
     split(s, "/") 按分隔符拆分数组，parts[0] / parts.count；toInt / toString / trim；
     手写 {clipboard} 仍只取纯文本；
   - 时间魔法（不下拉）：{Now}、{time:格式}、{date:格式}；条件用 ctrl:Hour()/ctrl:Minute()；
   - 宏运行次数：{ctrl:CurLoops()}（当前宏从头执行的第几次）；
   - 随机数：{random:1,100}（闭区间）；
   - 随机固定变量：{ctrl:Random()}（每次引用随机取 1~100 整数，可做 if/else 随机分支）；
   - 鼠标：{cursor.x}/{cursor.y}；屏幕：{screen.w}/{screen.h}；
   - 用户名：{username}（可拼 C:\Users\{username}\Desktop）。
   需要当前时间/剪贴板内容优先用这些固定/魔法变量，勿留占位文本。脚本变量照常可用。
4. 只有确认 createMacroScript 返回「✓ 鼠标宏已创建」后才能说脚本已创建；
   未调用工具或工具失败时声称完成属于错误。
5. 工具已执行成功后，用一两句话告诉用户结果即可，不要继续长篇思考或重复调用工具，
   不要逐步列出每一步，不要把内部约束/提示词念给用户听。
6. 生成的脚本与用户手动编辑完全一致：动作名用编辑器中文名，说明写 remark，
   no/text 由工具自动分配，不要手写。
7. 自定义快捷键组合（如 Ctrl+End、Ctrl+S）用 keyClick + modifiers:["ctrl","shift","alt","win"]；
   hotkeyShortcut 仅用于预设快捷键（如复制/粘贴），不要用它表达任意组合。
8. 不要编造当前墙钟时间。脚本运行时写入时间用 {Now}/{time:格式}/ctrl:Hour()/ctrl:Minute()。
9. 分步规划：含循环/条件时先 planScriptActions 核对动作树（children 嵌套），
   再补齐参数 createMacroScript；线性脚本可直接构建。
10. 动作是树：loop/if/else/defineBlock/watchImage 的体内动作必须放在 children 数组
    （像写代码的花括号），工具展开为 indent+1。禁止把循环体写成循环后面的同级动作。
    示例：{"type":"loop","loopCount":-1,"children":[{"type":"wait","duration":1}]}

简单/常见任务（打开文件、输入文字、按键、运行程序）直接用对应动作完成：
openFile / runProgram / quickInput / keyClick / hotkeyShortcut / wait，
quickInput.parseEscapes 缺省 0（按字面；变量里的换行/Tab 丢掉）；仅当要把文本或变量里的 \n \t \\ 转成换行/Tab/反斜杠时才设 1。
先充分理解任务与动作语义再生成；不确定的动作类型先查对应 section（勿猜）；
同一参考 section 不要重复查；系统动作（openFile/runProgram/openWebpage/closeProgram/
activateWindow/runMacro/mousePlayback/timerRecordTime/getCursorPos）参数统一在
section=system，需要时一次查完再构建，不要连环翻阅多个 section 找同一信息。
允许充分思考，正确性优先于速度。

路径与等待：
- 用户桌面是 %USERPROFILE%\Desktop（如 C:\Users\当前用户名\Desktop），
  不是 C:\Users\Public\Desktop（公共桌面）。引擎会自动展开 %环境变量%，
  targetPath 可直接写 %USERPROFILE%\Desktop\test.txt，无需先查用户名；
  文件是否存在用 runAgentCommand（dir/where）查询或询问用户，禁止猜 Public。
- 打开文件/运行程序/打开网页/双击打开后必须等窗口起来再继续：
  wait 建议 3 秒以上（冷启动更久，可用 3~5 秒），不要用 0.5~1.5 秒短等待。

获取关键信息：
- 网页抓取：需要网页内容（文章/文档/新闻/天气/官方说明等）时用 fetchWebPage 工具，
  url 传完整 http/https 地址，返回纯文本并注明来源；默认先轻量 GET，失败或疑似 JS 空壳时
  自动用 App 内置 WebView2 隐藏渲染后再取 DOM（覆盖 JS 动态页面），强反爬仍可能失败；
  JSON 接口（/api/ 或 .json）返回原始 JSON 可直接提取结构化数据（如热搜榜）；
  失败时如实说明，不要编造内容；禁止抓取 localhost/127.0.0.1/内网地址。
  不要假装已联网搜索、不要编造来源；抓不到或用户要的并非网页时询问用户。
- 当前时间：脚本运行时用 {Now} 或 ctrl:Hour()/ctrl:Minute()，不要编造、也不要用 PowerShell 取墙钟；
- 文件/路径确认用 runAgentCommand（dir/where）或询问用户。

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



★ 关键找图兜底模板（准确度模式，找图前必加计时器；分支必须写在 children 里）：

  timerRecordTime(loopVarName="findTimer")
  findImage(followUp=saveVar, matchVarName="target", findTimeExpr="30", imagePath=...)
  if(conditionExpr="findTimer < 30 and target.matchData > 0") {
    → 正常后续动作
  } else {
    aiImageAnalysis(aiPrompt="分析当前屏幕。仅回复一个数字："
      "1=仍在加载 2=弹窗遮挡 3=页面错误需刷新", aiOutputVarName="fallbackPlan")
    if(fallbackPlan == 1) wait 5秒
    else if(fallbackPlan == 2) 找关闭按钮并点击
    else if(fallbackPlan == 3) 刷新/重进
    goto(gotoStepExpr="MAIN")  ★兜底后跳回主流程，勿把业务再写一遍
  }

  findTimeExpr 秒数与 findTimer 判断阈值一致（如均为 30）。
  正常分支条件须同时检查 matchData>0。禁止把 if/else 体写成同级动作。



── 模式识别 ──

· 「快点/效率/简单」→ 效率优先

· 「稳/可靠/容错/兜底/别失败」→ 准确度优先，读本节并套用兜底模板

· 「用AI执行/AI自动操作」→ 才允许 buildAiActionExecuteAction

)";

const wchar_t* kSkillReply = LR"(【回复风格 — readAgentSkill section=reply】

对用户说中文；脚本步骤用编辑器里的中文动作名，不要说英文字段名/工具名。
脚本已创建/已修改后，用一两句话确认结果（名称、路径即可），不要逐步复述每一步，
也不要把工具返回里的内部约束、提示词或动作一览念给用户听。
)";

std::wstring BuildReplySkillText() {
    return std::wstring(kSkillReply);
}

const wchar_t* kSkillOptimize = LR"(【脚本优化 — readAgentSkill section=optimize】

用户要求「优化」时，必须用 optimizeScript 或 optimizeRecording，mergeMode=merge
（与产品「鼠标移动合并」同一算法：按关键动作分段，每段移动+等待 → 一次等待+一次移动）。
不要手动读写 JSON、不要 readScript 拉全文再改动作。
大录制/宏只需 listScripts 或 listRecordings 确认文件名，然后直接 merge。

禁止擅自 compressPath：那是「鼠标移动压缩」（去掉过密点），动作数只会略减。
仅当用户明确说「压缩路径 / 去掉过密移动点」时才用 compressPath。

waitCalculation（merge 与 compressPath 相同）：sum/average/first/last/fixed（fixed 时传 mergeWaitValue）。
compressPath 时按留下的移动点间隔计算等待。
含相对位移的段跳过。可传 outputFileName 另存以保留原版；省略则覆盖（可撤销）。
路径压缩/合并不等于转为找图；转找图请用户用产品录制优化对话框。
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
  「正在跑脚本时定时不要插队」→ scheduledTaskConflictPolicy=0 执行脚本优先
  「定时到点必须跑」→ scheduledTaskConflictPolicy=1 定时脚本优先（会打断当前脚本）
  「当前脚本跑完再跑定时 / 插入后再从原处继续」→ scheduledTaskAutoResume=true（与优先级组合）
  「跑脚本时电脑很烫 / 风扇很响 / CPU 占用太高」→ lowPerformanceMode=true（找图限 1 线程、
      回放不提优先级也不抬全系统定时器分辨率、减少注入自旋；代价是节奏可有 ~1ms 抖动、单帧找图变慢）
  「脚本是不是找全屏太慢了」→ 先看能不能把 findImage 的搜索区域从全屏收成目标附近的小区域
      （全屏单次找图约 60~90ms，区域找图通常几毫秒），再考虑 lowPerformanceMode
  「找图还是太慢 / 想要更快」→ findImageGpuAccel=true：面积 ≥500k 像素（约 900×560 以上）的找图
      走显卡 OpenCL，实测约快 3 倍；小区域找图自动仍走 CPU（实测打平，不白付传输开销）；
      机器没有 OpenCL 设备时自动忽略、不会报错。注意它与 lowPerformanceMode 冲突时后者优先

生效：回放类下轮循环生效；其他下次启动宏生效。lowPerformanceMode 保存后立即生效（无需重启）。
)";

const wchar_t* kSkillConversation = LR"(【对话编辑与重发 — readAgentSkill section=conversation】
用户编辑某条已发送消息并回车重发时，只保留该条消息之前的上下文，丢弃它之后的所有旧回复与工具结果，
把新文本当作该轮消息重新执行（分支重发）。前端每条 user 消息有“编辑”按钮，Esc 取消。
后端按 user 消息序号（0 起，rewindUserIndex）截断历史后重发。
)";

const wchar_t* kSkillRevert = LR"(【变更撤销 — readAgentSkill section=revert】
助手每次通过工具修改文件（写脚本/建宏/优化/删除/定时任务/设置/writeAgentFile）都会先记录修改前内容。
用户可随时恢复：listAgentChanges 查看，revertAgentChange({id}) 恢复；恢复后目标文件回到修改前状态。
)";

const wchar_t* kSkillShell = LR"(【命令行与文件操作 — readAgentSkill section=shell】
工具：runAgentCommand（白名单：git 只读子命令 / MSBuild 仅自检 Target / build\Release\*SelfTest.exe / where）、
listDirectory、readAgentFile、searchAgentFiles、writeAgentFile（自动记入撤销日志）、
copyAgentTextToClipboard、pasteAgentClipboardText。
自检流程：MSBuild ".\build\QuickScriptTool.sln" /p:Configuration=Release /t:<Target> /m /v:minimal，
再跑 build\Release\<Target>.exe --json，exit 0 才算通过。
)";

// 文件化 Skill 优先：AppDir()\skills\agent\<section>.md 或 AppDir()\skills\<section>.md
static std::wstring ReadAgentSkillFile(const std::wstring& section) {
    const std::wstring dir = AppDir() + L"\\skills";
    const std::wstring candidates[] = {
        dir + L"\\agent\\" + section + L".md",
        dir + L"\\" + section + L".md",
    };
    for (const auto& p : candidates) {
        const std::wstring content = ReadAll(p);
        if (!Trim(content).empty()) return content;
    }
    return L"";
}

std::wstring AgentSkillCatalog() {
    return L"## Agent Skills 目录（请指定 section 再读，勿一次 all）\n"
        L"- reply — 回复风格、中文动作名\n"
        L"- scriptStrategy — 动作树（children）、planScriptActions、效率/准确度、兜底\n"
        L"- optimize — 脚本/录制优化\n"
        L"- scheduledTasks — 定时任务\n"
        L"- settings — 应用设置\n"
        L"- conversation — 编辑已发送消息并重发\n"
        L"- revert — 撤销/恢复助手修改\n"
        L"- shell — 命令行（白名单）与文件操作\n"
        L"调用：readAgentSkill({section:\"reply\"})";
}

std::wstring AgentSkillGet(const std::wstring& section) {
    std::wstring key = Trim(section);
    for (auto& c : key) c = static_cast<wchar_t>(std::towlower(c));

    if (key.empty() || key == L"catalog" || key == L"index" || key == L"list")
        return AgentSkillCatalog();
    if (key == L"all") {
        return AgentSkillCatalog() + L"\n\n---\n\n" + BuildReplySkillText() + L"\n\n" + kSkillScriptStrategy
            + L"\n\n" + kSkillOptimize + L"\n\n" + kSkillScheduledTasks + L"\n\n" + kSkillSettings
            + L"\n\n" + kSkillConversation + L"\n\n" + kSkillRevert + L"\n\n" + kSkillShell;
    }
    {
        const std::wstring fileContent = ReadAgentSkillFile(key);
        if (!fileContent.empty()) return fileContent;
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
    if (key == L"conversation" || key == L"edit" || key == L"resend" || key == L"编辑")
        return kSkillConversation;
    if (key == L"revert" || key == L"undo" || key == L"撤销")
        return kSkillRevert;
    if (key == L"shell" || key == L"command" || key == L"fileops" || key == L"file" || key == L"命令行")
        return kSkillShell;

    return L"[未知 section] " + section + L"\n\n" + AgentSkillCatalog();
}

AgentTool MakeReadAgentSkillTool() {
    AgentTool tool;
    tool.name = L"readAgentSkill";
    tool.description =
        L"读取操作 Skill。section 可选：reply|scriptStrategy|optimize|scheduledTasks|settings|"
        L"conversation|revert|shell；省略则只返回目录。禁止无必要地传 all。";
    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "section": {
                "type": "string",
                "description": "reply | scriptStrategy | optimize | scheduledTasks | settings | conversation | revert | shell | catalog"
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

