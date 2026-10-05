@echo off
REM Fetch ONNX models used by the local-detection benchmark.
REM Keep this file ASCII-only (a zh-CN host reads .cmd as GB2312).
setlocal
cd /d "%~dp0"

set YOLOX_URL=https://github.com/opencv/opencv_zoo/raw/main/models/object_detection_yolox/object_detection_yolox_2022nov.onnx
set VIT_URL=https://github.com/opencv/opencv_zoo/raw/main/models/object_tracking_vittrack/object_tracking_vittrack_2023sep.onnx

if not exist yolox.onnx (
    echo [1/2] downloading yolox.onnx ...
    curl -sL --max-time 180 -o yolox.onnx "%YOLOX_URL%"
) else (
    echo [1/2] yolox.onnx already present
)

if not exist vit.onnx (
    echo [2/2] downloading vit.onnx ...
    curl -sL --max-time 180 -o vit.onnx "%VIT_URL%"
) else (
    echo [2/2] vit.onnx already present
)

for %%F in (yolox.onnx vit.onnx) do (
    if exist %%F (for %%A in (%%F) do echo     %%F  %%~zA bytes) else (echo     %%F  MISSING)
)
echo Done.
endlocal
