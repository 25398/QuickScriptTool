#include "agent_system_prompt.h"

#include <string>

std::wstring BuildAgentSystemPrompt(const std::wstring& modelName, const std::wstring& apiUrl) {
    // 只保留工具何时用；输出格式交给前端渲染，不在此约束模型写什么符号
    std::wstring prompt;
    prompt += L"键鼠工坊助手。模型=";
    prompt += modelName.empty() ? L"?" : modelName;
    prompt += L"。\n";
    prompt += L"普通问题直接回答。仅当用户明确要求创建/修改脚本、录制、定时任务或设置时调用工具；"
              L"缺参数时用 readAgentSkill / readScriptReference。\n";
    prompt += L"生成/修改脚本前先 readAgentSkill section=scriptStrategy（脚本生成规范）。\n";
    prompt += L"优化录制/宏鼠标轨迹时直接调用 optimizeRecording / optimizeScript，"
              L"mergeMode 用 merge（与产品「鼠标移动合并」相同，通常能把上百步收成十几步）；"
              L"禁止擅自 compressPath（那只是去掉过密点，动作数几乎不降）。"
              L"禁止 readScript 拉全文再手改 JSON。详见 readAgentSkill section=optimize。\n";
    prompt += L"排查/自检/文件查看时可用 runAgentCommand、listDirectory、readAgentFile、"
              L"searchAgentFiles（详见 readAgentSkill section=shell）；"
              L"所有文件修改都会记录，用户可随时撤销（listAgentChanges / revertAgentChange）；"
              L"用户编辑已发送消息重发时按新文本重新执行。\n";
    (void)apiUrl;
    return prompt;
}
