// image_var_util.h — 图片变量：路径判定、存盘、运行时解析与临时文件清理
#pragma once

#include <string>
#include <unordered_map>
#include <windows.h>

/// 含 \\ / 或盘符 X: → 视为文件路径；否则视为临时图片变量名。
bool LooksLikeFilePath(const std::wstring& s);

/// 确保 path 的父目录存在（可多级）。
bool EnsureParentDir(const std::wstring& path);

/// 按扩展名保存：无后缀/.bmp → BMP；.png/.jpg/.jpeg → WIC。
bool SaveHBitmapByExtension(HBITMAP bitmap, const std::wstring& path);

/// 宏运行期临时图片目录：{AppDir}\\scripts\\images\\_runtime
std::wstring ImageVarRuntimeDir();

/// 生成临时变量文件路径：_runtime\\{runId}_{varName}.bmp
std::wstring MakeImageVarTempPath(unsigned long long runId, const std::wstring& varName);

/// 解析「变量名或路径」为可 Load 的绝对路径。
/// - 路径样：绝对路径原样；否则走 ResolveImagePath
/// - 变量名：查 imageVars
std::wstring ResolveRuntimeImagePath(
    const std::wstring& token,
    const std::unordered_map<std::wstring, std::wstring>* imageVars);

/// 删除 map 中指向 _runtime 下的文件并清空 map。
void ClearImageVars(std::unordered_map<std::wstring, std::wstring>& imageVars);

/// 读取图片宽高（文件存在且可解码时返回 true）。
bool GetImageFileSize(const std::wstring& path, int& outW, int& outH);

/// 从锁屏位图裁剪或实时截取屏幕区域。
HBITMAP CaptureScreenOrFrozenRegion(int x1, int y1, int x2, int y2,
    HBITMAP frozenScreen, int frozenVirtX, int frozenVirtY);

/// 将截图写入「图片保存到」目标（路径或临时变量）。成功返回 true。
bool CommitSavedImage(HBITMAP bmp, const std::wstring& target,
    unsigned long long runId,
    std::unordered_map<std::wstring, std::wstring>& imageVars);
