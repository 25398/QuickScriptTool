#pragma once

#include <string>

#include "agent_core.h"

std::wstring AgentReferenceGet(const std::wstring& section);
AgentTool MakeReadScriptReferenceTool();

std::wstring AgentSkillGet(const std::wstring& section);
AgentTool MakeReadAgentSkillTool();
