#pragma once

#include <windows.h>
#include <dwrite.h>

/// 由 HFONT 获取（或创建并缓存）DirectWrite 文本格式；font 为空时返回默认格式。
IDWriteTextFormat* AcquireDWriteTextFormat(HFONT font);

void ClearDWriteFontCache();
