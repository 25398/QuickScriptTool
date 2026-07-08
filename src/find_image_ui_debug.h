#pragma once

#include <windows.h>
#include <string>

// 测试版诊断开关：1=开启（写日志 + 屏幕状态条），0=关闭
#ifndef FIND_IMAGE_UI_DEBUG
#define FIND_IMAGE_UI_DEBUG 0
#endif

#if FIND_IMAGE_UI_DEBUG

void FiDbgInit(HWND mainWnd);
void FiDbgShutdown();
void FiDbgSetMainWnd(HWND mainWnd);
void FiDbgLog(const wchar_t* event, const std::wstring& detail = L"");
void FiDbgLogFmt(const wchar_t* event, const wchar_t* fmt, ...);
std::wstring FiDbgHwndTag(HWND hwnd);
std::wstring FiDbgGrayButtonTag(HWND hwnd);
std::wstring FiDbgItemState(UINT state);
std::wstring FiDbgWindowChain(HWND hwnd, int maxDepth = 4);
void FiDbgLogGrayButtonLayout(const wchar_t* group, HWND btn);
void FiDbgBumpGrayDraw(HWND btn, UINT itemState, HWND hoverBtn, bool force = false);
void FiDbgOnGrayClick(HWND btn);
void FiDbgOnGrayHoverChanged(HWND oldBtn, HWND newBtn);

#else

inline void FiDbgInit(HWND) {}
inline void FiDbgShutdown() {}
inline void FiDbgSetMainWnd(HWND) {}
inline void FiDbgLog(const wchar_t*, const std::wstring& = L"") {}
inline void FiDbgLogFmt(const wchar_t*, const wchar_t*, ...) {}
inline std::wstring FiDbgHwndTag(HWND) { return L"(null)"; }
inline std::wstring FiDbgGrayButtonTag(HWND) { return L"(null)"; }
inline std::wstring FiDbgItemState(UINT) { return L"0"; }
inline std::wstring FiDbgWindowChain(HWND, int = 4) { return L""; }
inline void FiDbgLogGrayButtonLayout(const wchar_t*, HWND) {}
inline void FiDbgBumpGrayDraw(HWND, UINT, HWND, bool = false) {}
inline void FiDbgOnGrayClick(HWND) {}
inline void FiDbgOnGrayHoverChanged(HWND, HWND) {}

#endif
