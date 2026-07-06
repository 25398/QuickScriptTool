#include "agent_reference.h"



#include "utils.h"



#include <nlohmann/json.hpp>

#include <cwctype>



namespace {



using json = nlohmann::json;



const wchar_t* kRefFormat = LR"(【脚本文件格式】

脚本是 UTF-8 JSON，位于 scripts 目录（或 recordings 录制目录）。

顶层字段：

  scriptName, recordTime, durationSeconds, hotkeyText, hotkeyVk, hotkeyModifiers, actions[]



每个 action 对象包含公共字段 + 类型专用字段。软件保存时会写出全部字段；

AI 生成时至少必须写出 type 及该类型所需字段，其余可填默认值（见各动作说明）。



公共字段（每个 action 都有）：

  type        动作类型字符串（见下文）

  text        自定义显示名，通常留空 ""

  remark      备注，可写「待确认: …」

  no          序号，从 1 递增

  indent      缩进层级：0=顶层；if/loop/defineBlock 的子动作 = 父级 indent+1



缩进规则：

  loop / if / else / defineBlock 是容器，其内部动作 indent 比容器大 1。

  endLoop 与对应 loop 同级（indent 相同）。

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

  findUntilFound    1=循环查找直到找到（followUp=2 时应为 0）

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

  "text": "", "remark": "", "no": 1, "indent": 0,

  "imagePath": "images\\target.bmp",

  "matchThreshold": 65,

  "searchFullScreen": 1,

  "searchX1": 0, "searchY1": 0, "searchX2": 0, "searchY2": 0,

  "imageScale": 1, "imageScaleMin": 1, "imageScaleMax": 1,

  "findImageFollowUp": 2,

  "offsetX": 0, "offsetY": 0,

  "findUntilFound": 0,

  "matchVarName": "btnIcon",

  ...其余公共字段填默认值...

},

{

  "type": "if",

  "text": "", "remark": "", "no": 2, "indent": 0,

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



const wchar_t* kRefMouseKeyboard = LR"(【鼠标动作】

moveMouse:

  x, y              固定坐标（moveFromVar=0 时）

  randomX, randomY  随机偏移范围

  moveFromVar       1=来自变量表达式

  moveVarExprX/Y    表达式，如 "btn.x+10"、"anchor.x"、"{btnIcon}.y+5"



mouseClick / mouseDown / mouseUp:

  button            left(默认) / right / middle / x1 / x2

  clickCount        重复次数（mouseClick），默认 1

  duration          每次间隔秒数，默认 0.1

  randomDuration    随机附加等待，默认 0

  holdLeftCtrl 等   修饰键 0/1（mouseDown/Up/Click 可选）



scrollWheel:

  scrollVertical    1=垂直（默认）

  scrollHorizontal  1=水平

  scrollDirection   0=向上/左，1=向下/右

  scrollSteps       步数，默认 1

  clickCount/duration/randomDuration  重复与等待



wait:

  duration          等待秒数，默认 0.1

  randomDuration    随机附加秒数



【键盘动作】

keyClick / keyDown / keyUp:

  keyText           按键显示名，如 "A"、"Enter"、"F5"

  keyVk             虚拟键码（单字符可用 ASCII 码）

  holdLeftCtrl 等   修饰键组合

  clickCount/duration/randomDuration  keyClick 专用



hotkeyShortcut:

  shortcutPreset    预设索引 0~8：

    0=Ctrl+C, 1=Ctrl+V, 2=Ctrl+X, 3=Ctrl+S, 4=Ctrl+F,

    5=Alt+F4, 6=Win+D, 7=Win+R, 8=Ctrl+Alt+Delete

  clickCount/duration/randomDuration



quickInput:

  inputText         要输入的文本（支持 \n \r \t \\ 转义）

  charInterval      字符间隔秒数，默认 0.01

  clickCount/duration/randomDuration

)";



const wchar_t* kRefFlow = LR"(【流程控制】

loop:

  loopCount         循环次数，-1=无限循环

  loopFromVar       1=次数来自表达式

  loopVarExpr       次数表达式（loopFromVar=1 时）

  loopVarName       循环计数变量名（可选，用于 {变量名} 获取当前第几次）



endLoop:

  与 loop 配对，indent 与 loop 相同



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



stopMacro, lockScreenshot, unlockScreenshot:

  无额外必填字段



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

customText:   text 字段写显示内容

)";



const wchar_t* kRefAi = LR"(【AI 动作 — 必读】

以下四种动作必须使用专用工具构建（禁止用 buildScriptActions 手写）：
  buildGetCursorPosAction / buildAiTextAnalysisAction /
  buildAiImageAnalysisAction / buildAiActionExecuteAction
可用 listAiModels 查看已添加模型；图片分析与带截图的执行会自动选择识图模型。



getCursorPos — 获取当前光标位置

  matchVarName      变量名，默认 cursor

  执行后可用 {变量名}.x、{变量名}.y 在后续 moveMouse / if 中引用



aiTextAnalysis — AI 文字分析

  aiPrompt          ★必填：发给模型的分析要求（可含 {变量} 引用）

  aiOutputVarName   输出变量名，默认 aiResult

  aiOutputType      0=文本（默认）  1=数字

  aiModelName       模型名；专用工具会自动从已添加模型中选择（图片分析优先识图模型）



aiImageAnalysis — AI 图片分析

  字段同 aiTextAnalysis，另加：

  aiOutputVarName   默认 aiImgResult

  aiImageScale      截屏缩放 0.1~1.0，默认 1.0

  aiRegionByImage   1=先找图再按相对区域截屏

  aiTargetImagePath 锚定图片路径 images\xxx.bmp

  aiSearchX1/Y1/X2/Y2  截屏区域（不填且 aiRegionByImage=0 时全屏）



aiActionExecute — AI 动作执行

  aiPrompt          ★必填：要让 AI 完成的桌面操作任务描述

  aiModelName       模型名；专用工具按 aiWithImage 自动选择

  aiWithImage       1=附带屏幕截图（默认 1）

  aiRegionByImage / aiTargetImagePath / aiSearchX1~Y2  截图区域（同图片分析）

  aiMaxSteps        本动作内 AI 生成步骤上限，默认 10；-1=不限制

  aiConfirmExecute  1=执行前弹窗确认

  aiContextMode / aiTimeoutSec / aiFallbackValue



★ 典型流程 — 识图后 AI 分析再输入：

  1. findImage(followUp=2, matchVarName="anchor")

  2. aiImageAnalysis(aiPrompt="描述按钮文字", aiRegionByImage=1,

     aiTargetImagePath=同 anchor 图, aiSearchX1~Y2=相对区域)

  3. quickInput(inputText="{aiImgResult}")



★ 典型流程 — 纯文本 AI 结果写入变量后判断：

  1. aiTextAnalysis(aiPrompt="…", aiOutputVarName="summary")

  2. if(conditionExpr="summary >> 成功") → 子动作



不确定字段时：readScriptReference(section=ai) 或各 buildAi* 工具的参数说明

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

8. 循环：loop(count) → 子动作 → endLoop

9. 代码块：defineBlock → 子动作 → runBlock（主流程中跳过 defineBlock）

10. 录制优化：硬编码坐标改 findImage；加 wait；加 if 条件

11. AI 分析/执行：需要模型返回文字时用 aiTextAnalysis；需要看图时用 aiImageAnalysis；

    需要 AI 自动操作桌面时用 aiActionExecute；获取坐标用 getCursorPos

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

)";



const wchar_t* kRefActions = LR"(【动作类型索引】

基础：wait, moveMouse, mouseClick, mouseDown, mouseUp,

      keyClick, keyDown, keyUp, quickInput, hotkeyShortcut,

      scrollWheel, customText

流程：loop, endLoop, defineBlock, runBlock, if, else, stopMacro

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

        + kRefFormat + kRefActions + kRefFindImage + kRefTextRecognition

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

        + L"\n可用 section: all, format, actions, findImage, ocr, mouse, flow, ai,"

        L" conditions, variables, patterns, mistakes";

}



AgentTool MakeReadScriptReferenceTool() {

    AgentTool tool;

    tool.name = L"readScriptReference";

    tool.description =

        L"读取脚本格式 Skill。section: format|findImage|ocr|flow|ai|conditions|variables|patterns|mistakes|all。"
        L"动作 JSON 用 buildScriptActions 或 buildAi* 工具生成，勿手写。"
        L"优化/定时任务/设置/回复风格用 readAgentSkill。";

    tool.parameters_json = LR"({

        "type": "object",

        "properties": {

            "section": {

                "type": "string",

                "description": "all | format | actions | findImage | ocr | mouse | flow | ai | conditions | variables | patterns | mistakes"

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

const wchar_t* kSkillReply = LR"(【回复风格 — readAgentSkill section=reply】

思考与回复都会展示给用户。

规则：
1. 口语化中文纯文本，像聊天一样自然。
2. 禁止 Markdown：不要用井号标题、星号粗体、反引号、表格、短横线列表。
3. 禁止在思考或回复中出现任何英文字段名/工具名（如 enableCoordinateJitter、listScripts）。
   工具参数里的英文只是后台标识，对用户说中文含义即可。
4. 分点用 1. 2. 3. 或 · 开头。

常见翻译（心里对照，不要说英文）：
  坐标抖动、回放次数、随机间隔、定时任务、鼠标宏、键鼠录制、识图、图片分析
)";

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
3. 已有脚本则直接 createScheduledTask 指定 targetFile

频率：
  custom=单次（需年月日时分秒）
  daily=每天（时分）
  weekly=每周（时分+星期，「每周天」=周日）
  hourly=每小时（分）

默认创建鼠标宏类型任务，除非用户明确说是录制。
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
        return std::wstring(kSkillReply) + L"\n\n" + kSkillOptimize + L"\n\n"
            + kSkillScheduledTasks + L"\n\n" + kSkillSettings
            + L"\n\n可用 section: all, reply, optimize, scheduledTasks, settings";
    }
    if (key == L"reply" || key == L"style" || key == L"回复")
        return kSkillReply;
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
        L"section: reply|optimize|scheduledTasks|settings|all。"
        L"涉及优化、定时任务、改设置、回复风格时先读对应 section。";
    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "section": {
                "type": "string",
                "description": "reply | optimize | scheduledTasks | settings | all"
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

