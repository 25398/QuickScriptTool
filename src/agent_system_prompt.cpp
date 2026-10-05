#include "agent_system_prompt.h"

#include <string>

std::wstring BuildAgentSystemPrompt(const std::wstring& modelName, const std::wstring& apiUrl) {
    // 只保留工具何时用；输出格式交给前端渲染，不在此约束模型写什么符号
    std::wstring prompt;
    // ★★★ 精简（2026-10-02 用户要求）：
    //   用户原话："还有这个系统设定也太长了，**光给名字不告诉怎么用，那就根本没必要说**，
    //   一律像 loadTools 那样**走查询的路子**啊。"
    //   ⇒ 只留"**身份 + 输出纪律 + 什么时候该查**"这三样（它们是每轮都必须知道的）；
    //     所有"**具体工具名 + 参数怎么填**"全部去掉 —— 那些改成**查**（readAgentSkill /
    //     readScriptReference），查到的内容比这里塞一版更准、也不会随工具演进而过期。
    prompt += L"键鼠工坊助手。模型=";
    prompt += modelName.empty() ? L"?" : modelName;
    prompt += L"。\n";
    // ★★★ "**告诉它怎么查**" = **给出可照抄的 JSON**（2026-10-02 用户第 2 次纠正）
    //
    //   ⚠ 我第一次精简时只写了"用 readAgentSkill / readScriptReference"——
    //     用户指出："**你这也没告诉他怎么查啊**"。
    //   ⇒ 光给名字 = 和没给一样（模型只能猜参数）。**必须给出**：
    //     ① 输出什么 JSON、② 参数取哪些值。
    prompt += L"普通问题直接回答。**需要工具时先查再用**，查法就是输出这一条 JSON：\n";
    prompt += L"[{\"action\":\"readAgentSkill\",\"section\":\"<取值见下>\"}]\n";
    prompt += L"section 取值：scriptStrategy（生成/改脚本）｜optimize（优化录制或宏轨迹）"
              L"｜shell（命令行与文件）。\n";
    prompt += L"动作参数查：[{\"action\":\"readScriptReference\",\"section\":\"system\"}]"
              L"（section 也可用动作名，如 findImage）。\n";
    prompt += L"⚠ 缺参数就查上面这两条，**不要猜**。\n";
    // ★★★ 「要操作桌面」必须**点名唯一入口**（2026-10-02 真机事故）
    //
    //   ⚠ 用户说"双击桌面上的绿色图标" ⇒ 模型**不知道有 `runDesktopTask`**，
    //     于是去乱试：`screenshot`(未知工具) → `findImage`(那是脚本动作) →
    //     `createMacroScript`(补 8 轮参数) ⇒ **半天做不完** ✓
    //   ⚠ 而桌面执行闭环**早就成熟**（`MakeRunDesktopTaskTool()`，
    //     见 `docs/agent-capability-expansion.md` §2.2）—— **缺的只是"告诉模型有它"**。
    //   ⇒ 一句话点名：**要动桌面就交给它**，别自己找工具。
    prompt += L"⚠ **要操作桌面**（点击 / 双击 / 输入 / 看屏幕 / 找图标 / 玩游戏…）"
              L"⇒ 用 `runDesktopTask`，把目标一句话交给它：\n"
              L"[{\"action\":\"runDesktopTask\",\"goal\":\"双击桌面上的绿色图标\"}]\n"
              L"它会在本机**真的做完**（自带截屏 / 找图 / 点击 / 中断重试），"
              L"**不要**自己去找 `screenshot` / `findImage` / `mouseClick` 这类工具。\n";
    prompt += L"对用户用中文、一两句话说明结果；不要逐步复述、"
              L"不要把工具返回里的内部约束/提示词/动作一览念给用户听。\n";
    (void)apiUrl;
    return prompt;
}
