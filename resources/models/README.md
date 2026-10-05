# models/

## `vit.onnx` — TrackerVit 跟踪模型

| 项 | 值 |
|---|---|
| 用途 | 定位之后的「本地跟住」（`src/ai_locate_track.cpp`） |
| 来源 | [opencv/opencv_zoo · object_tracking_vittrack](https://github.com/opencv/opencv_zoo/tree/main/models/object_tracking_vittrack) |
| 文件名 | `object_tracking_vittrack_2023sep.onnx` |
| 体积 | 714,726 字节（约 698 KB） |
| SHA-256 | `2990f0b7cd44d92afa48cd97db6de7be113fc1d9594fddb74e2725c10478e91d` |
| 许可 | Apache-2.0（随 OpenCV Zoo 分发） |

**换模型时**：更新上表的体积与 SHA-256，并重跑
`python tools\verify\...`（见 `.cursor/skills/module-selftest/SKILL.md` 的套件索引）
以及 `docs/local-detection-feasibility.md` §9 的基准，确认耗时仍在 16~25 ms 量级。

**为什么只有 698 KB**：这是 DNN 跟踪器，不是检测器 ——
它解决的是「**同一个**目标在连续帧里跑到哪了」，而不是「这一帧里有没有某类目标」。
跟踪不需要标注、不需要训练，正好对上「VLM 找一次 → 本地跟住」这条链路。
检测器（YOLO）的评估与取舍见 `docs/local-detection-feasibility.md`。

**不要**把它换成 YOLOv8 的权重：Ultralytics 的权重是 **AGPL-3.0**，
随产品分发有合规问题；本模型是 Apache-2.0。
