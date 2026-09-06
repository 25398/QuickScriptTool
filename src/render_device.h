#pragma once
// ──────────────────────────────────────────────────────────────────
// render_device.h — Direct2D / DirectWrite 工厂与设备生命周期
// ──────────────────────────────────────────────────────────────────

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

enum class RenderBackend {
    Gdi,
    Direct2D,
};

/// 初始化渲染设备（在 wWinMain 启动时调用一次）
bool InitRenderDevice();

/// 释放渲染设备（进程退出前调用）
void ShutdownRenderDevice();

/// Direct2D 是否可用（工厂创建成功）
bool IsDirect2DAvailable();
bool IsDirectWriteAvailable();

/// 获取/设置首选后端（Direct2D 不可用时自动回退 GDI）
RenderBackend GetPreferredRenderBackend();
void SetPreferredRenderBackend(RenderBackend backend);
/// 按设置 preferDirect2D 应用首选后端；D2D 不可用时静默回退并最多记一次调试日志
void ApplyPreferredRenderBackend(bool preferDirect2D);

ID2D1Factory* GetD2DFactory();
IDWriteFactory* GetDWriteFactory();
