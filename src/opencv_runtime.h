#pragma once

#include <string>

/// OpenCV 运行时：产品壳 DELAYLOAD opencv_world4100.dll。
/// 杀软隔离或缺文件时仍可进壳；找图/依赖 cv:: 的路径必须先查 OpenCvAvailable()。
bool TryInitOpenCv(std::wstring* reason = nullptr);
bool OpenCvAvailable();
void MarkOpenCvUnavailable(const wchar_t* reason);
const wchar_t* OpenCvUnavailableMessage();
