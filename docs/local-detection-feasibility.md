# 本地视觉检测（YOLO / DNN Tracker）集成可行性评估

> **续篇**：本文成文时 **RF-DETR 尚不存在**（Roboflow 2025-11 发布）。
> 关于 RF-DETR（Apache-2.0、DINOv2 骨干）与「用别人数据集做优化」的评估见
> **[`rf-detr-yolo-integration-assessment.md`](rf-detr-yolo-integration-assessment.md)**。
> 该文对本文的 §2（「零新依赖」）、§11.1（许可证表）、§11.2（mAP 差异不重要）三处结论有**更新**。

> ⚠⚠ **撤销声明（先读这一段）**：本文章的 **§10「落地记录」已经作废** ——
> `TrackerVit` 跟踪（`src/ai_locate_track.*`）连同定位模板缓存（`src/ai_locate_cache.*`）、
> 布局记忆（`src/ai_ui_layout.*`）已按「**引擎只做感知 + 执行 + 如实回执，绝不替模型做决定，
> 也绝不保留跨帧世界状态**」的原则**全部撤销**（见 `docs/ai-action-exec-optimization.md` §45/§46）。
> 撤销理由不是性能，而是**正确性**：拿一次带噪的感知去写「目标还在老地方」这种世界状态，
> 出错的形态是**静默点到错的地方**，而它省下的只是识图钱。
> **§0~§9 的实测数字仍然有效**（模型体积、单帧耗时、噪声底 84~110px 都是本机量出来的，
> 对以后任何「要不要引入某个视觉原语」的判断都还有用）—— 但**不要据此重建 §10 那套接线**。
> 若将来仍要「跟住目标」，正确做法是把它做成**模型可调用的工具**（模型要跟就跟），
> 而不是引擎在背后替模型维持一个会话。

> 起因：用户问「轻量集成 YOLO 识图，拓展脚本效率（AI 动作执行 / 找图）」。
> 本文**全部数字均为本机实测**（复现方式见 §9），非引用值。结论与既有
> `docs/ai-action-exec-optimization.md` §21.2「暂不引入 YOLO」不冲突，但**优先级排序要改**：
> 该引入的是 **OpenCV 自带的 DNN 跟踪器（767 KB，零新依赖）**，YOLO 检测器排在它后面。

---

## 0. 一句话结论

| 问题 | 答案 |
|---|---|
| 依赖成本「轻量」吗？ | **是，而且比预期更轻**：产品已发的 `opencv_world4100.dll`（61.7 MB）**自带 dnn 模块**（`readNetFromONNX` / `blobFromImage` / `NMSBoxes` 全在），也自带 **tracking 模块**（`TrackerVit` / `TrackerNano` / `TrackerDaSiamRPN`）。**引入 ONNX 推理不需要任何新的运行时 DLL。** |
| YOLO 值得做吗？ | **值得，但落点只有一个**：替代「VLM 识图」而不是替代「模板匹配」。 |
| 最该先做的是什么？ | **不是 YOLO**，是 `TrackerVit` 跟踪：**698 KB 模型、16~23 ms/帧（43~62 FPS）、无需训练、无需标注**，精确命中 §21.4 那条一直没做的「VLM 找一次 → 本地跟住」。 |
| GPU 加速走得通吗？ | **OpenCV dnn 的 GPU 路在这台机器上不通**：无 CUDA 后端；OpenCL 后端内核编译失败（`CL_BUILD_PROGRAM_FAILURE`），实测比 CPU **慢 6 倍**。要 GPU 只能引 ONNX Runtime + DirectML。 |

---

## 1. 实测结果（本机，可复现）

环境：Intel 13 代（16 线程）· RTX 4060 Laptop 8 GB + Intel UHD（双显卡）·
OpenCV 4.10.0 world（`D:/OpenCV/opencv/build`，即产品实际链接的那一份）。

### 1.1 单帧推理耗时

| 方案 | 模型体积 | 实测单帧（avg，3 次运行区间） | 备注 |
|---|---|---|---|
| **OpenCV 模板匹配（现状）** | 0（脚本自带截图） | **毫秒 ~ 数十 ms** | 产品既有；`FindTemplateOnScreenMulti` |
| **`TrackerVit` 跟踪更新** | **698 KB** | **16.2 ~ 23.3 ms（43~62 FPS）** | create 18~20 ms / init 4.2 ms，**一次性** |
| YOLOX-s ONNX @640，16 线程 | 35.8 MB | **257 / 297 / 356 ms** | min 237~283 ms |
| YOLOX-s ONNX @320，16 线程 | 35.8 MB | **65 / 94 ms** | min 56.6 ms |
| YOLOX-s ONNX @640，**1 线程**（低性能模式） | — | **883 / 1015 ms** | 产品低性能模式会把 OpenCV 线程压到 1 |
| YOLOX-s ONNX @320，1 线程 | — | **254 ms** | 低性能模式下 YOLO 直接不可用 |
| YOLOX-s ONNX @640，**OpenCL** | — | **1608 ms** | ⚠ 内核编译失败后退化，比 CPU 慢 6 倍 |
| **yolov8n @640（FLOPs 折算，未实测）** | 12.8 MB fp32 / 3.4 MB int8 | **≈85 ms** | YOLOX-s 26.8 GFLOPs → yolov8n 8.7 GFLOPs |
| **yolov8n @320（折算，未实测）** | 同上 | **≈21 ms** | 这个数量级才够用 |
| **VLM 识图（现状）** | 0 | **约 10 800 ms** | `docs/ai-action-exec-optimization.md` §28.1 实测 |

### 1.2 抓帧与预处理（不是瓶颈）

| 段 | 实测 |
|---|---|
| GDI `BitBlt` + `GetDIBits` 抓 1707×960 | **27 ~ 52 ms** |
| `blobFromImage` 1707×960 → 640×640 | **4.1 ~ 7.3 ms** |
| `blobFromImage` → 320×320 | 1.3 ~ 2.1 ms |
| （对照）产品实测「截屏 + JPEG 编码 + base64」2560×1440 | **797 ~ 859 ms**（§28.1） |

⇒ 预处理与抓帧**都不是瓶颈**；`blobFromImage` 比现有 JPEG 编码链路便宜两个数量级。
**唯一的瓶颈是推理本身。**

### 1.3 OpenCL / GPU 盘点（为什么 GPU 路不通）

```
OpenCL available=1
  platform[0] NVIDIA CUDA
    device[0] NVIDIA GeForce RTX 4060 Laptop GPU | GPU | 24 CU | 8188 MB | no-fp16
  platform[1] Intel(R) OpenCL HD Graphics
    device[0] Intel(R) UHD Graphics | GPU | 64 CU | 12999 MB | fp16-ok
  OpenCV 默认选中: NVIDIA GeForce RTX 4060 Laptop GPU
```

OpenCV 选中的是 **RTX 4060**，但 `DNN_TARGET_OPENCL` 跑出来：

```
OpenCL program build log: dnn/dummy
Status -11: CL_BUILD_PROGRAM_FAILURE
-cl-no-subgroup-ifp
Error in processing command line: Don't understand command line argument "-cl-no-subgroup-ifp"!
```

NVIDIA 的 OpenCL 驱动不认识 OpenCV 生成的编译参数 ⇒ 内核建不起来 ⇒ 每个算子退化为
「尝试 → 失败 → 回落 CPU」⇒ **1608 ms，比纯 CPU 慢 6 倍**。

**结论**：`cv::dnn` 在本机**没有任何可用的 GPU 后端**（这份 OpenCV 无 CUDA 后端，
OpenCL 后端与 NVIDIA 驱动不兼容）。**GPU 是 YOLO 方案能不能「实时」的前提，
而它需要额外的运行时（ONNX Runtime + DirectML，约 20 MB DLL），这就不再是「零新依赖」了。**

---

## 2. 「轻量」这一半是对的：依赖成本 ≈ 0

```
readNetFromONNX     3 处   ← ONNX 导入器在
blobFromImage      23 处   ← 预处理在
NMSBoxes           10 处   ← 后处理在
softNMSBoxes        2 处
DetectionModel     79 处   ← cv::dnn::DetectionModel 在
TrackerVit         19 处   ← DNN 跟踪器在
TrackerNano        19 处
TrackerDaSiamRPN   18 处
```
（`strings` 扫 `build/Release/opencv_world4100.dll` 的结果）

产品**已经发了** 61.7 MB 的 `opencv_world4100.dll`，dnn 与 tracking 都在里面。
所以：

- **不加任何 DLL**：C++ 侧 `#include <opencv2/dnn.hpp>` 直接用，ONNX 模型文件随包/按需下载。
- 体积代价 = **模型文件本身**：TrackerVit **698 KB**；yolov8n fp32 12.8 MB / int8 3.4 MB。
- 对照基线：安装包 **207 MB**（`dist/QuickScriptTool-1.3.3.exe`）⇒ 加 3.4 MB ≈ **+1.6%**，
  加 698 KB ≈ **+0.34%**。**体积完全不是问题。**
- 分发走既有模式（与 OCR Python、HID 驱动同款）：**不进默认包，设置里按需下载**。

---

## 3. 但收益点比想象窄：YOLO 到底替代谁？

这是本次评估最重要的一条。**YOLO 的价值完全取决于它替代掉的那一步有多贵。**

| 被替代对象 | 现状成本 | YOLO 成本 | 净收益 |
|---|---|---|---|
| **VLM 识图** | **10 800 ms** | 21~85 ms（yolov8n @320/640） | ✅ **快 130~500 倍**，这是唯一有意义的落点 |
| OpenCV 模板匹配（找图） | 毫秒~数十 ms | 65~356 ms | ❌ **更慢**，纯负收益 |
| TrackerVit 跟踪 | 16~23 ms | 65~356 ms | ❌ **更慢** |
| OCR 文字索引 | 已落地（§20.9） | — | ❌ 无收益，OCR 已覆盖 |

**所以「用 YOLO 拓展找图效率」这个方向本身不成立** —— 模板匹配已经是毫秒级，
YOLO 在任何输入尺寸下都比它慢。YOLO 只能用在**模板匹配根本做不到的场景**：

1. 目标有**缩放/旋转/形变/半透明/发光**，模板匹配的 NCC + 像素终审必然失配；
2. 目标是**动画帧**（同一物体的不同姿态），模板匹配要截几十张图，YOLO 一个类就够；
3. 目标是**类别而非个体**（「任意一个敌人」「任意一个关闭按钮」）——模板匹配做不到。

而这三条，恰好都是**要训练**的：通用 YOLO 只认 COCO 80 类，
PvZ 的僵尸、Excel 的按钮都不在里面 —— 这正是 §21.2 反对它的原始理由，**该理由今天依然成立**。

### 3.1 开放词表检测器能救吗？（回应 §21.2 的反对理由）

技术上能：YOLO-World / YOLOE / GroundingDINO 支持**文本提示零样本检测**，
「不认识本品目标」这条不再绝对成立。但：

- 模型明显更大（YOLO-Worldv2-S 数十 MB + 需要 CLIP 文本编码器或离线文本嵌入）；
- 零样本精度**低于** VLM grounding，而本品已经有 **OCR 文字索引 + UIA 控件树 + 周期网格推断**
  三条更便宜的替代路径（§21.2 已论证）；
- 且它要解决的场景（「大量纯图标、无文字、无控件树」）至今**没有真实案例**。

⇒ **维持 §21.2 的结论：开放词表检测器现在不做。** 等真遇到那个场景再说。

---

## 4. 真正值得做的三条路（按性价比排序）

### Tier 0 —— `TrackerVit` 跟住目标（**建议立刻做**）

**做什么**：把 `src/ai_locate_cache.h` 的「模板匹配再捕获」升级为
「模板匹配（首选，精确）+ `TrackerVit`（模板失配时兜底）」。
定位一次（VLM / OCR / UIA 任意来源）→ 本地 43~62 FPS 跟住 → 只在跟踪丢失时才重新识图。

**为什么它比 YOLO 优先**：

| | TrackerVit | YOLO |
|---|---|---|
| 模型体积 | **698 KB** | 3.4 ~ 12.8 MB |
| 单帧 | **16~23 ms** | 21 ~ 85 ms（折算） |
| 需要训练/标注 | **不需要**（框一次就行） | **需要** |
| 新依赖 | **无** | 无（CPU）/ 有（GPU） |
| 对「同一目标持续跟踪」 | **这就是它的本职** | 用检测器做跟踪是浪费 |

**与既有架构的契合度极高**：
- `ai_locate_cache` 已有一套成熟的安全门闩（窗口键一致 / 高分 / 唯一命中 / 距缓存点不远 /
  N-of-M 迟滞 / 点击后画面没变就作废）。**跟踪器直接接进这套门闩即可**，不用新造判据。
- §21.4 未做的第 1 条就是这件事：「让宿主在游戏前台时把刚定位到的目标自动转成
  `findImage`/`findColor` 跟随后续动作」。
- `TrackerVit::getTrackingScore()` 提供**跟踪置信度**，正好可以当「该不该回落识图」的判据。

**已知坑**（写在这里省得后面再踩）：
- `TrackerVit` **不在 `opencv2/opencv.hpp` 之外的地方**，声明在 `opencv2/video/tracking.hpp`
  （已由 `opencv.hpp` 间接包含，不用额外 include）；
- `Params.net` 是模型路径，`backend` / `target` 默认值即可（**别设 OPENCL**，见 §1.3）；
- **`cv::TrackerCSRT` / `KCF` / `MOSSE` 在这份 world 包里不存在**（strings 计数为 0），
  §2.2 那条「CSRT 实测 <1 FPS，不是答案」的结论不用再验了，直接排除。

### Tier 1 —— 可插拔本地检测器（YOLO，**排在 Tier 0 之后**）

**做什么**：定义 `ILocalDetector` 抽象 + OpenCV dnn ONNX 后端，**只在「自训练类目标」上启用**。
使用形态：脚本作者/用户标注几十张图 → 训一个 yolov8n → 导出 ONNX 放脚本目录 →
脚本里写 `detectObjects: class=zombie` → 本地 21~85 ms 出框 → 直接点击。

**为什么它排在 Tier 0 之后**：它需要**训练数据**，而 Tier 0 不需要。
先做 Tier 0 拿数据（跟踪失败的那些帧天然就是难例），再决定要不要上检测器。

**规模评估**：`ILocalDetector` 接口 + OpenCV dnn 后端 + YOLOv8 输出解码（`1×84×8400` → 框）
+ NMS + 模型管理，约 **400~600 行**；模型管理复用既有「设置里下载可选组件」管线。

### Tier 2 —— OmniParser 式图标候选框（**现在不做**）

用检测器找「像可交互元素」的框，再交给模型/OCR 命名（Set-of-Mark）。
价值场景（未知界面、大量纯图标、无文字、无控件树）**至今没有真实案例**；
已有 UIA 控件树（桌面）与周期网格推断（卡槽/草坪）两条更便宜的替代。
**维持 §21.2 结论。**

---

## 5. 明确不建议做的

| 不做 | 原因（实测） |
|---|---|
| `cv::dnn` 的 `DNN_TARGET_OPENCL` | 内核编译失败，**比 CPU 慢 6 倍**（1608 ms vs 257 ms） |
| 用 YOLO 替代 `findImage` 模板匹配 | **更慢**（65~356 ms vs 毫秒~数十 ms），且精度更差（模板匹配有像素终审） |
| 用 YOLO 替代 OCR 文字索引 | OCR 已落地且更便宜，无收益 |
| 引 ONNX Runtime + CUDA EP | 数百 MB（CUDA/cuDNN），与「轻量」直接冲突 |
| 把模型打进默认安装包 | 违背既有约定（大组件走设置里下载），且无必要 |

---

## 6. 工程约束（真要动手时必读，全是本仓已有前科）

1. **低性能模式必须硬禁用本地推理**：实测 1 线程时 YOLOX-s @320 要 **254 ms**、@640 要 **883~1015 ms**。
   与 `SyncImageMatchThreadBudget()` 同款处理：低性能模式开启 ⇒ 直接回落模板匹配。
2. **OpenCV 符号守卫**：任何调用 OpenCV 的地方前面必须有 `OpenCvAvailable()`，
   且守卫要在**任何 `cv::Mat` 出现之前**（`cv::Mat` 的构造/析构本身就是 OpenCV 符号）。
   漏了会抛 `0xC06D007E` 把进程当场带走（没日志、没结束音）。
3. **导出 exe / 脚本包的能力体检必须同步扩两张表**：
   `ActionNeedsOpenCv`（`src/script_package.cpp`）+ `ScriptPackageSelfTest::scan_dependency_matrix`。
   **只改一处矩阵会变红**；漏判 = 导出后运行期崩 / 静默失效（已有两次前科）。
4. **模型文件必须校验**（FNV 或 SHA-256）：下载半截 / 被替换 / 被杀软改写都要能识别并回落。
   `src/script_package.h` 已有 FNV 尾标机制可复用。
5. **模型缺失必须优雅回落**，不是报错：走「检测器不可用 → 模板匹配 → VLM」的降级链，
   与现有 OCR 后端 `OcrBackendAttemptOrder()` 同款思路。
6. **新增自检 suite**（如 `LocalDetectorSelfTest`），必须包含
   ① 无模型时的回落断言 ② 低性能模式禁用断言 ③ 输出框解码的逐格断言（纯函数）。
7. **不要动 `input_timeline_scheduler` / 注入层**：本次改动全在「感知」侧，
   与「后台+软输入」卡顿那套（`PrepareSoftInputFast` 等）无关，别顺手改。

---

## 7. 建议分期与验收判据

| 期 | 内容 | 验收判据 |
|---|---|---|
| **P0**（半天） | 把 §9 的基准测试纳入 `tools/local_detection_bench/`，作为后续任何改动的**对照基线** | `bench.exe` 可复现本文全部数字 |
| **P1**（2~3 天） | `TrackerVit` 接进 `ai_locate_cache`：模板匹配优先、跟踪兜底、跟踪分低则作废回落 | ① 自检全绿；② 实测「同一目标连点 20 次」的**识图次数**从 20 降到 ≤2；③ 跟踪丢失时能正确回落（不许静默点错） |
| **P2**（1 周） | `ILocalDetector` + OpenCV dnn ONNX 后端 + yolov8n；只在自训练类目标上启用 | ① 模型缺失时优雅回落；② 导出 exe 后能力体检正确（两张表同步）；③ 低性能模式禁用 |
| **P3**（视需求） | 决定要不要引 ONNX Runtime + DirectML 拿 GPU | **先实测 DirectML 的实际耗时**，再决定是否值得 +20 MB 依赖 |

**P1 的收益预估**：现状「一次识图 10.8 s」，命中定位缓存后本地毫秒级。
跟踪器把「缓存失效后必须重新识图」的那些轮次也省掉 —— 动态画面（游戏残影、视频）里
模板匹配频繁失配，正是跟踪器最能顶上的地方。**这是投入产出比最高的一步。**

---

## 8. 风险清单

| 风险 | 等级 | 缓解 |
|---|---|---|
| 跟踪器漂移到错误目标并静默点错 | **高** | 复用 `ai_locate_cache` 全套门闩 + `getTrackingScore()` 阈值 + 「点击后画面没变即作废」 |
| 模型文件分发（下载失败/被篡改） | 中 | FNV/SHA 校验 + 缺失回落 |
| 用户机器无独显（只有核显） | 中 | 反正只走 CPU；GPU 路本来就不通 |
| 低性能模式用户被拖慢 | 中 | 硬禁用（§6.1） |
| 导出 exe 能力体检漏判 | **高** | 两张表同步改（§6.3），有 `ScriptPackageSelfTest` 兜底 |
| 模型版权 / 许可 | 低 | TrackerVit = Apache-2.0（OpenCV Zoo）；YOLO = AGPL-3.0 ⚠ **商用需注意** |

> ⚠ **许可提示**：Ultralytics YOLOv8 是 **AGPL-3.0**。若要随产品分发其权重/衍生模型，
> 需评估合规（或改用 Apache-2.0 的 YOLOX / NanoDet / PP-YOLOE）。**TrackerVit 无此问题。**

---

## 9. 复现方式

```bash
# 1. 取模型（约 36 MB）
tools\local_detection_bench\fetch_models.cmd

# 2. 构建（务必清代理，否则 MSB6001）
cd tools\local_detection_bench
env -u HTTPS_PROXY -u https_proxy -u HTTP_PROXY -u http_proxy \
  "/c/Program Files/CMake/bin/cmake.exe" -S . -B build -G "Visual Studio 17 2022" -A x64
env -u HTTPS_PROXY -u https_proxy -u HTTP_PROXY -u http_proxy \
  "/c/Program Files/CMake/bin/cmake.exe" --build build --config Release

# 3. 跑（把 opencv_world4100.dll 放到同目录）
cp D:/OpenCV/opencv/build/x64/vc16/bin/opencv_world4100.dll .
./build/Release/bench.exe .
```

**注意事项**：
- `main.cpp` 含中文注释，CMake 里必须加 **`/utf-8`**；否则 zh-CN 宿主按 GB2312 读，
  中文注释末字节撞上 `0x5C` 会把下一行 `#include` 吞进注释，报出
  「`cv` 不是类或命名空间名称」这种**完全误导**的错误。
- 未做 DPI 感知 ⇒ `GetSystemMetrics` 返回虚拟化尺寸（本机 1707×960），
  产品实际是 2560×1440。**这不影响推理耗时**（推理输入尺寸固定），
  只影响抓帧与 `blobFromImage`（后者实测仅 4~7 ms）。
- 本文未实测的项已逐条标注「折算 / 未实测」，别当成实测值引用。

---

## 10. 落地记录（P1：TrackerVit 接进定位缓存）—— ⚠ 已撤销，只作历史

> **本节描述的实现已全部删除**（`ai_locate_track.*` / `ai_locate_cache.*` / `ai_ui_layout.*` 三个模块
> 都不在代码里了）。保留这一节是为了记住「我们试过、量过、为什么放弃」：
> 跟踪器本身没问题，**问题在于引擎替模型维持了一个跨帧会话** —— 那等于引擎自己决定目标在哪。
> 复现方式与实测数字见 §10.3；**接线不要照抄**。

### 10.1 改了什么

| 文件 | 改动 |
|---|---|
| `src/ai_locate_track.h/.cpp`（新） | 跟踪模块：纯函数判决层 + `TrackerVit` 薄封装 + 模型发现 + 缺失静默禁用 |
| `src/engine/engine_script_run.cpp` | ① VLM 定位成功、存模板后**同时起跟踪会话**；② 模板匹配没定下来时**让跟踪器顶一帧**；③ 缓存作废时**一起结束会话** |
| `src/macro_execute_tools.cpp` | `ResetAiActionSessionState()` 里随 `AiLocateCacheClear()` 一起 `AiTrackEndAll()` |
| `resources/models/vit.onnx`（新） | 跟踪模型 714,726 字节（Apache-2.0，见同目录 README） |
| `CMakeLists.txt` | `qst_copy_product_runtime` 把模型拷到 `<产物目录>\models\`；`AiActionRouterSelfTest` 也拷一份（真模型冒烟用） |
| `tools/package_release.ps1` / `tools/package_webview_portable.ps1` | 把模型收进 dist / 便携包 |
| `tools/ai_action_router_selftest.cpp` | 新用例 `locate_track` / `locate_track_unavailable` / `locate_track_session` / `locate_track_first_step` / `locate_track_smoke`（判决表逐格 + 会话生命周期 + 坐标口径 + 降级无声 + 真模型冒烟） |

### 10.2 三条设计决定（都是刻意的，改之前先读）

1. **跟踪器只做「模板匹配的替补」，不做主力**。
   模板匹配有像素级终审、精度更高；只有当它**没能定下来**（抖动 / 分数不够 / 没命中）时
   才问跟踪器。顺序反过来会让「大概对」顶掉「精确对」。

2. **绝不用跟踪框回写模板缓存**。
   一旦回写，模板就永远认同跟踪器的结论，「跟错」会变成**自证循环**，
   而「模板对不上 → 回识图」这条独立证据就消失了。保留原模板 ⇒
   跟错了下一轮模板照样对不上、照样回识图。

3. **判「跟错」的主判据是「单帧跳变」，不是「距固定点的绝对漂移」**。
   目标本来就可能真的在移动（游戏里的敌人、滚动列表里的项），
   拿固定期望点当基准会把**正常移动**判成跟错。所以：
   - `maxStepJumpPx = 80`（**主判据**，跟错时会先表现为一次大跳）；
   - `maxDriftPx = 400`（**兜底**，只是防跑飞）。
   另外低分**容忍单帧**（可能只是被挡了一下），连续 2 帧才判跟丢 ——
   立刻回落的代价是 10.8s，而跟踪框是连续演化的，代价不对等。

### 10.3 ⚠ 坐标口径：`Session::box` 一律**屏幕坐标**（踩过一个致命 bug）

`AiTrackBegin` 收的是**屏幕坐标**、`AiTrackStep` 返回的也是屏幕坐标，`Session::box` 必须同口径。
但 Begin 里要先把它转成帧内坐标喂给 `tracker->init()`：

```cpp
cv::Rect box(boxX1 - frameX, boxY1 - frameY, bw, bh);   // 帧内，给 OpenCV 用
box &= cv::Rect(0, 0, bgr.cols, bgr.rows);
...
s->box = cv::Rect(box.x + frameX, box.y + frameY, box.width, box.height);  // ★存回屏幕坐标
```

**漏掉最后这一步的后果**（初版就是这样，注释还写着「屏幕坐标」）：

| 环节 | 用帧内坐标当屏幕坐标会怎样 |
|---|---|
| 搜索区 `rx1 = max(vx, s->box.x - expand)` | 拿帧内坐标去和**虚拟屏幕原点**比 ⇒ 抓到的是**屏幕左上角**那块，目标根本不在里面 |
| `prevCx = s->box.x + w/2` | 单帧跳变基准错位 ⇒ 首帧必判 `jumped` |
| `driftPx = |框中心 − 期望点|` | 期望点是真屏幕坐标 ⇒ 必然 > `maxDriftPx` ⇒ 判 `drifted` |

⇒ **每个会话第一步就死**（`jumped` 或 `drifted`），回落识图。**跟踪功能等于没接上**，
而且日志上看起来只是「跟踪不可用 → 回落识图」，像正常降级。

⚠ **最坑的是自检会全绿**：`locate_track` 判的是纯函数（跟坐标无关），
`locate_track_session` 只要求「最终出现非 Use 且会话自动结束」——
**第一步就死恰好满足它**。所以专门加了 `locate_track_first_step`：
静止画面上**首帧必须能跟住**，返回点落在初始框 200px 内，**且 `越界=0`**。
**A/B 实测**：退回这一行 ⇒ `命中 0/3；越界=3；首帧非 use：unavailable（抓屏失败）`、suite 红；
修好后 ⇒ `命中 2~3/3；越界=0`、绿（**命中数不是确定性指标**，见 §10.3.1 末）。

#### 10.3.1 断言必须把画面**钉住**，不能赌桌面静止

`AiTrackStep` **自己抓屏**（没有帧参数），所以这条断言最初只能抓**实时屏幕**当模板和帧源。
后果：它实际上在赌「用户桌面上那 3 个位置恰好是静止的」——
**用户前台在放视频时，屏幕中心那个位置 3/3 全失败**（实测长期只过 2/3）。

那是**用例缺陷**，不是回归；而按仓库纪律，环境依赖型断言的修法是**钉住**，
**不是**放宽判定（把「≥1 个位置」改成更松），也**不是**改成 `SKIP`
（`SKIP` 会让守门员在动画桌面上**永远不生效** = 等于没断言）。

修法：给模块加一个**仅测试用**的帧源接缝（与 `SetFetchOverrideForTest` /
`SetAiForegroundBrowserOverrideForTest` 同惯例）：

```cpp
// src/ai_locate_track.h
typedef HBITMAP (*AiTrackFrameSourceFn)(int x1, int y1, int x2, int y2, void* user);
void SetAiTrackFrameSourceForTest(AiTrackFrameSourceFn fn, void* user);  // nullptr = 真实抓屏
```

自检侧：拍一张区域 → 把它当作**冻结画面**，帧源按请求裁它的子区
（`FrozenFrame` / `FrozenFrameSource`）。于是「当前帧」就是模板本身 ⇒
静止是**构造出来的**，与用户桌面在放什么无关。

`FrozenFrame::outOfRange` 顺带成了口径的**直接**判据：搜索区是按 `s->box` 算的，
若 box 存成帧内坐标，请求矩形会整块落到冻结区**外面** ⇒ `越界=3` 一眼定位。
（比原来的「坐标跑飞：返回(x,y) 期望(x,y)」更早、更明确。）

⚠ **但「命中数」不是确定性的 —— 别把它当指标。** 冻结只保证**帧与模板同源**，
模板本身仍取自**实时屏幕**（内容逐次不同）。实测连跑 5 次（每格诊断是后加的）：

```
命中 3/3；越界=0；每格=[use 22%/5px][use 24%/4px][use 27%/6px]
命中 2/3；越界=0；每格=[use 16%/11px][jumped:跳变 94px 漂移 94px 分 17%][use 28%/5px]
命中 2/3；越界=0；每格=[use 21%/5px][use 19%/44px][jumped:跳变 84px 漂移 84px 分 15%]
命中 2/3；越界=0；每格=[use 23%/56px][jumped:跳变 100px 漂移 100px 分 38%][use 15%/4px]
命中 3/3；越界=0；每格=[use 17%/57px][use 20%/59px][use 25%/1px]
```

⇒ 判据是 **`≥1 且 越界=0`**（`越界` 恒定 0，`命中数` 在 2~3 之间跳）。

⚠ 顺带一条教训：**「命中 3/3」是我在只有一次观测时写进文档的过度断言**，
下次实跑就变 2/3。**单次观测不能写成"确定性"** —— 要么多跑几次，要么写区间。

#### 10.3.2 ★顺带量出了 `TrackerVit` 的**定位噪声底**（`maxStepJumpPx=80` 可能偏严）

上表那些失败格有个共同点：**`跳变 == 漂移`（94/94、84/84、100/100）**。
跳变基准是「上一次的框中心」、漂移基准是「期望点」，两者相等 ⇒ 说明**框根本没动**，
只是 tracker 给出的框中心离初始中心偏了 84~100px。

**关键：这时帧是冻结的、模板就是它本身 —— 画面完全没变。**
所以这 84~100px 是**模型自身的定位噪声**，不是坐标错、也不是目标在动。

⇒ 而 `maxStepJumpPx` 默认 **80**，正好卡在噪声底之下 ⇒ 小框（本例 80×80）会**偶发被判 jumped**、
白回落一次识图（每次回落 ≈10.8s VLM，代价极高）。
噪声大致随框尺寸缩放 ⇒ **绝对像素阈值对小框偏严**。

可选修法（**均未实施**，需先评估对「锁到别的物体」检测力的影响）：
① 阈值改成**相对**量（如 `跳变 > 1.5 × 框宽`）；② 按框尺寸分段；③ 保留绝对阈值但调大。

⚠ 这是**放宽**安全判据，不是修 bug。按仓库纪律，改之前**必须**先加一条反向用例
（「锁到别的物体仍能判出」），否则等于把安全阀拆了。

**判据：模块的公开坐标口径只允许一种。** 内部为了喂第三方库做临时转换可以，
但**存回结构体的那一行必须转回来**，并且要有一条断言直接验「返回值落在预期位置附近」——
只验「判决种类」的断言抓不到口径错。

### 10.4 ⚠ 必须钉住的一条：模型路径编码

**OpenCV 的 `readNet*` 只认 ANSI(GBK) 路径，不认 UTF-8。** 实测：

```
ANSI codepage = 936  (GBK)
[ascii] readNetFromONNX(UTF-8)  = THROW
[ascii] readNetFromONNX(CP_ACP) = OK
[cjk  ] readNetFromONNX(UTF-8)  = THROW
[cjk  ] readNetFromONNX(CP_ACP) = OK      ← 纯中文目录也 OK
```

注意**连「纯英文目录」的 UTF-8 也失败** —— 因为完整路径里的用户名段含中文。
所以 `ai_locate_track.cpp` 用 `WideToAcp()`，**不能用项目惯用的 `ToUtf8()`**；
用错的后果是「模型明明在、跟踪器永远起不来」这种最难查的静默失效。
自检里的「真模型冒烟」就是这条的守门员。

### 10.5 降级行为（缺模型 / 无 OpenCV 时）

| 环节 | 行为 |
|---|---|
| `AiTrackAvailable()` | 返回 false + 可读理由（指明是缺 `vit.onnx` 还是 OpenCV） |
| `AiTrackBegin()` | 直接返回 false，调用方不写任何分支 |
| `AiTrackStep()` | 无会话 ⇒ `Unavailable`，且**出参清零**（调用方不会拿到脏坐标去点） |
| 引擎 | 全部日志与行为**与改动前逐字一致** |

即：**没有模型的老安装包/便携包，跑起来和以前完全一样**，不会报错也不会崩。

### 10.6 验收判据（P1 完成的标准）

- [x] `AiActionRouterSelfTest` 全绿（**195** 用例）：`locate_track`（判决表逐格）、
      `locate_track_unavailable`、`locate_track_session`（会话生命周期）、
      `locate_track_first_step`（**坐标口径**，见 §10.3）、`locate_track_smoke`（真模型）
- [x] **权威回归入口** = `tools/run_all_selftests.ps1`（`Tier=logic`，**21 个套件**）：**21/21 全绿**
      ⚠ **不要自造 `for e in build/Release/*Test*.exe` 这种循环** —— 它会把**交互层**
      （`WindowModeSelfTest` / `InjectionSelfTest` / `VirtualHidSelfTest`）一起拉进来 ⇒ **假红**。
      （实测踩过：三个套件报红，但其产物是 Sep 19 / Aug 3 的旧二进制、**根本没被本次重建**、
      源码零处引用本模块 ⇒ 与改动无关，纯属我的循环写错了。）
      `VisionGroundingSelfTest` 在 `$LogicSuites` / `$InteractiveSuites` **两个清单里都没有**
      （需外部视觉模型 env），要单独跑。
- [ ] 真机 A/B：同一目标连点 20 次，**识图次数从 20 降到 ≤2**
      （看 `[诊断] 定位跟踪命中：…（模板失配但跟踪可用，省一次识图）` 出现几次）
- [ ] 跟踪失败时日志出现 `定位跟踪不可用：<原因>（分 …% 跳变 …px 漂移 …px 存活 …ms）`
      —— 这一行是判断「为什么回识图」的唯一入口，**不要**只看「回识图」三个字
- [ ] 点击无反应时，缓存与跟踪会话**同时**作废（日志两行相邻）
- [ ] ⚠ 若日志里**每轮**都是「定位跟踪不可用：jumped」⇒ 先查 §10.3 的坐标口径，
      别去调 `maxStepJumpPx`（那是拿阈值掩盖口径错）

---

## 11. YOLO 各版本：仓库 / 权重 / 接口 / **许可证边界**与选型

> 起因：用户明确要「不同版本的仓库、权重、接口和许可证边界」+「精度、速度、硬件与部署环境的选择建议」。
> **本节是硬约束**：本项目是**闭源商业软件**（Inno Setup 打包分发），许可证不是「尽量注意」而是**能不能用**。

### 11.1 许可证边界（先看这一张，再谈技术）

| 版本 | 归属 | 仓库 | 许可证 | 本项目可否用 |
|---|---|---|---|---|
| YOLOv5 | Ultralytics | `ultralytics/yolov5` | **AGPL-3.0** | ❌ |
| **YOLOv6** | 美团 | `meituan/YOLOv6` | **GPL-3.0** | ❌ |
| **YOLOv7** | WongKinYiu | `WongKinYiu/yolov7` | **GPL-3.0** | ❌ |
| **YOLOv8** | Ultralytics | `ultralytics/ultralytics` | **AGPL-3.0** ⚠ 2023 年曾是 Apache-2.0，**2024 年中改为 AGPL** | ❌ |
| **YOLOv9** | WongKinYiu | `WongKinYiu/yolov9` | **GPL 系**（AGPL-3.0 / GPL-3.0 各方口径不一，实测 LICENSE 为 GPL 系） | ❌ |
| **YOLOv10** | 清华 THU-MIG | `THU-MIG/yolov10` | **AGPL-3.0** | ❌ |
| YOLO11 / YOLO26 | Ultralytics | `ultralytics/ultralytics` | **AGPL-3.0** | ❌ |
| YOLO-World | 腾讯 | `AILab-CVC/YOLO-World` | **GPL-3.0** | ❌ |
| **YOLOX** | 旷视 | `Megvii-BaseDetection/YOLOX` | **Apache-2.0** | ✅ |
| **DAMO-YOLO** | 阿里达摩院 | `tinyvision/DAMO-YOLO` | **Apache-2.0** | ✅ |
| PP-YOLOE | 百度 | `PaddlePaddle/PaddleDetection` | **Apache-2.0** | ✅ |
| RT-DETR | 百度 | `PaddleDetection` / `lyuwenyu/RT-DETR` | **Apache-2.0** | ✅ |
| YOLO-MS | 华为 | `HVision-NKU/YOLO-MS` | **Apache-2.0** | ✅ |
| NanoDet | RangiLyu | `RangiLyu/nanodet` | **Apache-2.0** | ✅ |
| YOLO-NAS | Deci | `Deci-AI/super-gradients` | **专有，禁止商用** | ❌ |

**⚠ AGPL / GPL 的三个陷阱（都真实存在，别赌）**：

1. **「我自己从头训练就不算」——错。** 只要你**用了 Ultralytics 的代码/框架**训练，
   或**用了它发布的预训练权重**做起点，AGPL 就跟着你的产物走。Ultralytics 官方明确：
   连它发布的**预训练权重本身**都是 AGPL-3.0 ⇒ **把 `yolov8n.onnx` 打进安装包就是分发 AGPL 产物**。
2. **「我只在本地跑、不联网就不算」——错。** AGPL 的**网络交互条款**只是多一条触发路径，
   本项目走的是**分发**（安装包）这条更基础的路 ⇒ 无论联不联网都已触发。
3. **「学术仓 = 宽松许可」——错，且最容易踩。** v9（WongKinYiu）、v10（清华）看起来是论文配套仓，
   但 LICENSE 是 **GPL 系 / AGPL**。**判许可证只看仓库根的 LICENSE 文件，不看项目出身。**
4. 补充：**GPL-3.0 与 AGPL-3.0 对本项目的区别 = 0** —— 两者都在「分发」时触发完整源码开放义务，
   本项目要分发，所以**两个都不能用**。别把精力花在区分它们上。

⇒ **可选的只有 Apache-2.0 / MIT 那一列。** 对本项目最有意义的是 **YOLOX**（旷视，全系 Apache-2.0，
ONNX 导出成熟）与 **DAMO-YOLO**（阿里，同量级精度更高）、**NanoDet-Plus**（极小、纯 CPU 友好）。

### 11.2 各版本技术特点（用户列表逐条）+ 为什么它们对本项目**不是选型依据**

| 版本 | 招牌技术 | 技术亮点 | 对本项目的实际意义 |
|---|---|---|---|
| YOLOv6 | 美团 · **重参数化主干（RepVGG 式）**、硬件友好 | 工业部署导向，推理时把多分支融合成单路 ⇒ 同精度更快 | ⚠ **GPL-3.0 出局**。技术思路（重参数化）可借鉴，代码不能用 |
| YOLOv7 | **ELAN 高效长程聚合网络** + **复合模型缩放** | 当时 SOTA，缩放策略有理论依据 | ⚠ **GPL-3.0 出局** |
| YOLOv8 | **Anchor-Free** + **解耦头（decoupled head）** + 统一多任务 + Ultralytics 生态 | **工程体验最好**（训练/导出/部署一条龙），生态最全 | ⚠ **AGPL-3.0 出局**，且是**最诱人也最危险**的一个（生态好用，容易不知不觉引进去） |
| YOLOv9 | **PGI 可编程梯度信息** + **GELAN** | 解决深度网络信息瓶颈，小模型上提升明显 | ⚠ **GPL 系出局** |
| YOLOv10 | 清华 · **一致双分配（consistent dual assignment）** ⇒ **NMS-Free** + 轻量注意力 | **端到端无 NMS**，省掉后处理、延迟更稳 | ⚠ **AGPL-3.0 出局**；「NMS-Free」这个**特性**可由 **RT-DETR（Apache-2.0）** 取得 |

**★ 关键结论：对本项目来说，上表的「技术亮点」全部不是选型依据。**
理由（本机实测，见 §3 表）：

- 本项目要检测的场景是**自训练的少数类**（如「僵尸」「某个按钮」），
  **不是 COCO 80 类的通用检测** ⇒ v8 的「统一多任务」、v10 的「轻量注意力」都换不来收益；
- 本项目**只能跑 CPU**（§1.3：无 CUDA 后端、OpenCL 后端内核编译失败且慢 6 倍）
  ⇒ 各版本之间的 mAP 差异，远不如**把输入尺寸从 640 降到 320**（21 ms vs 85 ms，4 倍）来得重要；
- **NMS-Free** 是唯一有真实工程价值的差异（省后处理、延迟稳定），但它决定的是
  「用不用 RT-DETR」，**不决定「用 v8 还是 v10」** —— 而 v8/v10 都不可用。

### 11.3 选型建议（精度 / 速度 / 硬件 / 部署）

**硬约束（先钉死，否则选型没意义）**：

| 约束 | 结论 | 依据 |
|---|---|---|
| 硬件后端 | **只能 CPU**（OpenCV dnn） | §1.3：`DNN_TARGET_OPENCL` 内核编译失败，实测**比 CPU 慢 6 倍**；无 CUDA 后端 |
| 运行时依赖 | **零新增**（复用已发的 `opencv_world4100.dll` 的 dnn 模块） | §2 |
| 模型格式 | **ONNX**（`readNetFromONNX`）；⚠ 路径必须 **GBK/CP_ACP**，不认 UTF-8 | §10.4 |
| 精度口径 | 只关心**自训练类**的召回/精度，不看 COCO mAP | §3 |
| 模型体积 | 走「设置里下载可选组件」管线，**不打进默认安装包** | §5 |

**按场景选型**：

| 场景 | 推荐 | 理由 |
|---|---|---|
| **商用安全 + 尽量轻**（首选） | **YOLOX-Nano / Tiny**、**NanoDet-Plus** | Apache-2.0；模型 1~5 MB 级；ONNX 导出成熟；CPU 上快 |
| 想要**更好的精度/速度折中** | **DAMO-YOLO-T/S**（阿里，Apache-2.0） | 同量级下精度更高（NAS 搜出的结构），仍是 Apache-2.0 |
| 要**端到端 NMS-Free**、延迟稳定 | **RT-DETR**（Apache-2.0） | 无 NMS；但模型更大、CPU 上更慢 ⇒ **本项目不优先**，除非后处理抖动真成问题 |
| **绝对不要** | Ultralytics 全家（v5/v8/11/26）、v6/v7/v9/v10、YOLO-World、YOLO-NAS | AGPL / GPL / 专有 ⇒ 闭源商业分发直接违规 |

**部署路径（与本项目既有架构对齐）**：

1. 训练：用 **YOLOX**（Apache-2.0）在标注数据上训 → 导出 ONNX；
2. 运行时：`cv::dnn::readNetFromONNX(GBK 路径)` + `blobFromImage` + 手写输出解码
   （YOLOX 输出为 `1×N×85`，无 anchor）+ `cv::dnn::NMSBoxes`（**OpenCV 自带，见 §0**）；
3. 接入点：实现 `ILocalDetector`（§4 Tier 1），**只替代 VLM 识图**（10 800 ms → 21~85 ms），
   **不替代模板匹配**（模板匹配是毫秒级，YOLO 更慢，见 §3）；
4. 规模：接口 + 解码 + NMS + 模型管理 ≈ **400~600 行**（§4 Tier 1 评估不变）。

**★ 顺序仍然是：先 Tier 0（`TrackerVit`，已落地），再 Tier 1（检测器）。**
许可证把 v6/v7/v8/v9/v10 全部排除后，**Tier 1 的可选项只剩 Apache-2.0 那一列**，
而那一列里没有一个能改变「先做跟踪、后做检测」的顺序 —— 因为**跟踪不需要训练数据，检测需要**。

### 11.4 「理解动」这件事，检测器**本身**做不到（与 §3 呼应）

- **YOLO 是检测器（detector），不是跟踪器（tracker）**：单帧输入、单帧输出，
  **没有时间维度** ⇒ 它**无法**回答「这个东西在往哪走」；
- 让它「跟踪」的通行做法是 **detect-then-associate**：
  逐帧检测 + **数据关联**（IoU / 卡尔曼 + 匈牙利匹配）⇒ 这才是 ByteTrack / OC-SORT / SORT 干的事，
  **检测器只是它们的前端**；
- 所以「用 YOLO 让 AI 看懂目标移动」这个说法**把两件事混成了一件**：
  - **「画面里有什么、在哪」= 检测**（YOLO 系，需训练）
  - **「这个东西接下来会去哪」= 跟踪 + 运动模型**（`TrackerVit` / ByteTrack，**不需要训练**）
- **本项目已经拥有了后者的原语**（`src/ai_locate_track.cpp` 的 `TrackerVit` 会话，16~23 ms/帧）——
  用户想要的「不用重复找图也能理解界面」，**正是 Tier 0 已经在做的事**。
  缺的不是检测器，是下面 §11.5 的三层。

### 11.5 要让 AI「理解动」，真正缺的三层（按性价比排序，都不需要 YOLO）

| 层 | 现在有吗 | 缺什么 | 成本 |
|---|---|---|---|
| ① **运动状态层** | ❌ 只存了 `Session::box`（位置），**没存速度** | 存 `prevBox` + 算 `v = Δ中心/Δt`、朝向、**用卡尔曼/恒速模型预测下一帧位置** ⇒ 才能回答「往哪走、下一步在哪」 | **低**（几十行，纯逻辑，可自检） |
| ② **多目标关联** | ❌ `Sessions()` 是**单目标**（每 key 一个框） | 检测器出 N 个框 → IoU/匈牙利匹配到上一帧的 N 个框 → 每个目标一条轨迹（ByteTrack 式） | 中（需要检测器 = Tier 1） |
| ③ **检测器重捕获** | ❌ 丢失后回落 **VLM 识图 10 800 ms** | 用本地检测器在**同类别**上重新找到目标 ⇒ 把「跟丢的代价」从 10.8 s 降到 21~85 ms | 中（Tier 1） |

**★ ① 是现在就该做的，且它顺带解决了本次自检暴露的那个真问题**：
实测 `TrackerVit` 在真实屏幕内容上的**定位噪声有 84~110px**（§10.3.2），
而初始框只有 80×80、`maxStepJumpPx=80` ⇒ **小框会偶发被误判 `jumped`** ⇒ 白付一次 VLM。
**根因是「拿单帧位置差分当速度」** —— 单帧位置噪声直接变成「跳变」。
加了运动模型（卡尔曼平滑）后，判据就从「这一帧跳了多少」变成「**与预测位置的偏差**」，
**噪声被模型吸收，阈值也不用再靠猜**。这比调 `maxStepJumpPx` 正确得多
（§10.6 最后一条已明确：**别拿阈值掩盖口径/模型问题**）。

### 11.6 一句话回答用户的三个问题

| 问题 | 答案 |
|---|---|
| YOLO 对**识别动态移动目标**有优势吗？ | **没有**。YOLO 是单帧检测器，**没有时间维度**，「动」不是它能表达的概念。它的优势在**类别语义**（「任意一个敌人」）和**外观剧变下的重捕获**，不在运动。 |
| 能让 AI 在**执行过程中理解「动」**吗？ | **能，而且本项目已经在做**（`TrackerVit` 就是「不重跑找图也能跟住目标」）。**但「理解动」比「跟住」多一步：要存速度/朝向/预测位置** —— 这是 §11.5 ① 那一层，**不需要 YOLO**。 |
| 那 YOLO 到底该不该上？ | **该上，但排在后面、且只能从 Apache-2.0 那一列选**（YOLOX / DAMO-YOLO / NanoDet）。它的落点是**替代 VLM 识图**（10.8 s → 21~85 ms）和**多目标关联**，**不是**替代找图、**更不是**用来「理解动」。顺序：**Tier 0 跟踪（已做）→ ① 运动状态层 → Tier 1 检测器**。 |

