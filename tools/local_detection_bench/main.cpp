// QST 本地推理可行性实测：用「产品里已有的那一个 opencv_world4100.dll」
// 实测 (1) 屏幕抓帧+预处理 (2) YOLOX/YOLO 系 ONNX 前向 (3) TrackerVit 跟踪更新。
#include <opencv2/opencv.hpp>
#include <opencv2/core/ocl.hpp>
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <string>

using Clock = std::chrono::high_resolution_clock;
static double Ms(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

static cv::Mat GrabScreen() {
    const int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    HDC hdc = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, w, h, hdc, x, y, SRCCOPY);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    cv::Mat bgra(h, w, CV_8UC4);
    GetDIBits(mem, bmp, 0, h, bgra.data, &bi, DIB_RGB_COLORS);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, hdc);
    cv::Mat bgr;
    cv::cvtColor(bgra, bgr, cv::COLOR_BGRA2BGR);
    return bgr;
}

static void BenchDnn(const std::string& path, int inSize, int target, const char* tag,
                     const cv::Mat& frame, int iters) {
    printf("\n--- %s (in %dx%d, target=%s) ---\n", tag, inSize, inSize,
           target == cv::dnn::DNN_TARGET_OPENCL ? "OPENCL" : "CPU");
    cv::dnn::Net net;
    auto t0 = Clock::now();
    try {
        net = cv::dnn::readNetFromONNX(path);
    } catch (const std::exception& e) {
        printf("readNetFromONNX FAILED: %s\n", e.what());
        return;
    }
    net.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
    net.setPreferableTarget(target);
    auto t1 = Clock::now();
    printf("load=%0.1fms\n", Ms(t0, t1));

    cv::Mat blob;
    auto tb0 = Clock::now();
    cv::dnn::blobFromImage(frame, blob, 1.0 / 255.0, cv::Size(inSize, inSize),
                           cv::Scalar(), true, false, CV_32F);
    auto tb1 = Clock::now();
    printf("blobFromImage(整屏 %dx%d -> %dx%d)=%0.1fms\n", frame.cols, frame.rows,
           inSize, inSize, Ms(tb0, tb1));

    double best = 1e9, sum = 0;
    cv::Mat out;
    for (int i = 0; i < iters; ++i) {
        auto a = Clock::now();
        net.setInput(blob);
        out = net.forward();
        auto b = Clock::now();
        const double ms = Ms(a, b);
        sum += ms;
        if (ms < best) best = ms;
    }
    printf("forward x%d: avg=%0.1fms min=%0.1fms\n", iters, sum / iters, best);
    printf("out dims=%d shape=", out.dims);
    for (int d = 0; d < out.dims; ++d) printf("%d%s", out.size[d], d + 1 < out.dims ? "x" : "");
    printf("\n");
}

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : ".";
    printf("OpenCV %s  (build=%s)\n", CV_VERSION, cv::getBuildInformation().empty() ? "?" : "world");
    printf("threads=%d\n", cv::getNumThreads());

    // 显卡/OpenCL 设备盘点：决定「OpenCV dnn 能不能走 GPU」
    try {
        printf("OpenCL available=%d\n", cv::ocl::haveOpenCL());
        cv::ocl::setUseOpenCL(true);
        std::vector<cv::ocl::PlatformInfo> plats;
        cv::ocl::getPlatfomsInfo(plats);
        for (size_t i = 0; i < plats.size(); ++i) {
            printf("  platform[%zu] %s\n", i, plats[i].name().c_str());
            for (int d = 0; d < plats[i].deviceNumber(); ++d) {
                cv::ocl::Device dev;
                plats[i].getDevice(dev, d);
                printf("    device[%d] %s | type=%s | computeUnits=%d | globalMem=%0.0fMB | dnn=%s\n",
                       d, dev.name().c_str(),
                       dev.type() == cv::ocl::Device::TYPE_GPU ? "GPU" : "OTHER",
                       dev.maxComputeUnits(),
                       dev.globalMemSize() / 1048576.0,
                       dev.isExtensionSupported("cl_khr_fp16") ? "fp16-ok" : "no-fp16");
            }
        }
        cv::ocl::Device cur = cv::ocl::Device::getDefault();
        printf("  OpenCV 默认选中: %s\n", cur.name().c_str());
    } catch (const std::exception& e) {
        printf("OpenCL probe failed: %s\n", e.what());
    }

    cv::Mat frame;
    {
        auto a = Clock::now();
        frame = GrabScreen();
        auto b = Clock::now();
        printf("GDI 抓整屏 %dx%d = %0.1fms\n", frame.cols, frame.rows, Ms(a, b));
    }
    if (frame.empty()) { printf("screen grab failed\n"); return 1; }

    BenchDnn(dir + "/yolox.onnx", 640, cv::dnn::DNN_TARGET_CPU, "YOLOX-s 640", frame, 8);
    BenchDnn(dir + "/yolox.onnx", 320, cv::dnn::DNN_TARGET_CPU, "YOLOX-s 320", frame, 8);
    {
        // 低性能模式：产品里 OpenCV 线程预算会被压到 1（SyncImageMatchThreadBudget）
        cv::setNumThreads(1);
        printf("\n[低性能模式 threads=1]\n");
        BenchDnn(dir + "/yolox.onnx", 640, cv::dnn::DNN_TARGET_CPU, "YOLOX-s 640", frame, 5);
        BenchDnn(dir + "/yolox.onnx", 320, cv::dnn::DNN_TARGET_CPU, "YOLOX-s 320", frame, 5);
        cv::setNumThreads(16);
    }

    // TrackerVit：定位一次 -> 本地跟住
    printf("\n--- TrackerVit 跟踪 ---\n");
    try {
        cv::TrackerVit::Params p;
        p.net = dir + "/vit.onnx";
        auto a = Clock::now();
        auto tracker = cv::TrackerVit::create(p);
        auto b = Clock::now();
        printf("create=%0.1fms\n", Ms(a, b));
        cv::Rect box(frame.cols / 2 - 60, frame.rows / 2 - 60, 120, 120);
        a = Clock::now();
        tracker->init(frame, box);
        b = Clock::now();
        printf("init(首帧)=%0.1fms\n", Ms(a, b));
        double best = 1e9, sum = 0;
        cv::Rect r;
        for (int i = 0; i < 10; ++i) {
            a = Clock::now();
            tracker->update(frame, r);
            b = Clock::now();
            const double ms = Ms(a, b);
            sum += ms;
            if (ms < best) best = ms;
        }
        printf("update x10: avg=%0.1fms min=%0.1fms  -> 约 %0.0f FPS\n", sum / 10, best,
               1000.0 / (sum / 10));
        float sc = 0.f;
        printf("last box=(%d,%d,%d,%d)\n", r.x, r.y, r.width, r.height);
    } catch (const std::exception& e) {
        printf("TrackerVit FAILED: %s\n", e.what());
    }
    return 0;
}
