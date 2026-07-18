# cpp_wood_defect v2.0 — 基于 TensorRT-YOLO 的木板瑕疵检测系统

## 概述

| 项目 | 定位 |
|------|------|
| `cpp_wood_defect/` v2.0 | 产线端到端木板瑕疵检测系统，**以 TensorRT-YOLO 库为推理后端** |
| `TensorRT-YOLO/` | 通用 YOLO 推理库（开源），作为项目的依赖库使用 |

> v2.0 重构：删除了自行实现的 `trt_engine`（TensorRT 引擎管理）和 `preprocessor`（图像预处理），
> 全部由 TensorRT-YOLO 库接管。项目只保留独有的产线业务逻辑。

---

## 架构：v1.0 → v2.0 变化

```
v1.0（自己写推理）:
  相机 → Preprocessor(CLAHE+resize+归一化+HWC→CHW) → trt_engine(ONNX解析+TRT构建+enqueueV2)
       → Postprocessor(YOLO解码+NMS+规则+画框) → 显示/PLC

v2.0（TensorRT-YOLO 推理）:
  相机 → CLAHE(仅增强) → InferEngine → Postprocessor(规则+画框+保存) → 显示/PLC
               ↑              ↑                    ↑
           main.cpp       TensorRT-YOLO 内部完成:     纯业务逻辑
          5行OpenCV       预处理+推理+解码+NMS        YOLO格式零接触
```

### 文件变化

| 文件 | v1.0 | v2.0 |
|------|------|------|
| `trt_engine.h/cpp` | ~360行，手动管理 TRT builder/runtime/context/buffer | **已删除**，替换为 `infer.h`（15行 header-only） |
| `preprocessor.h/cpp` | ~100行，CPU resize + CLAHE + 归一化 + HWC→CHW | **已删除**，CLAHE 改为 main.cpp 里 5 行函数 |
| `postprocessor.h/cpp` | ~400行，含 YOLO 解码 + 手写 NMS + 规则 | **精简为 100行**，只剩规则+画框+保存 |
| `infer.h` | 不存在 | **新增**，封装 `trtyolo::DetectModel` |
| `main.cpp` | ~350行 | **精简为 100行** |
| `config.h` | ~80行，含 ONNX/FP16/workspace/CLAHE 等模型构建参数 | **精简为 30行**，去除所有 TRT 构建参数 |

---

## 项目依赖

```
cpp_wood_defect v2.0
├── TensorRT-YOLO（libtrtyolo.so）  ← 推理：预处理 + TRT引擎 + NMS解码
├── OpenCV                         ← CLAHE + 显示 + 画框
├── 海康 MVS SDK                   ← 相机采集
└── CUDA Toolkit                   ← TensorRT-YOLO 间接依赖
```

---

## 数据流详解

```
[1] HikvisionCamera::read()
    └─ 海康 MVS SDK 采集 → Bayer→BGR
       输出: cv::Mat (1920x1080 BGR uint8)

[2] enhance() — main.cpp 内 5 行函数
    └─ BGR→LAB → CLAHE(L通道) → LAB→BGR
       输出: cv::Mat (1920x1080 BGR uint8, 增强后)

[3] InferEngine::detect() — infer.h header-only
    └─ trtyolo::DetectModel::predict()
        内部: letterbox → 归一化 → BGR→RGB → TRT推理 → 解码+NMS
       输出: trtyolo::DetectRes (boxes/scores/classes，坐标已在原图空间)

[4] Postprocessor::process() — 规则引擎+画框（100行）
    ├─ convertResults: DetectRes → Defect（置信度过滤）
    ├─ severity: 规则引擎判定 ok/warn/ng
    └─ draw: 画框+标签
       输出: std::vector<Defect>

[5] 决策 + 显示 + 保存
    ├─ is_ng → GPIO 剔除信号（预留）
    ├─ cv::imshow → 实时画面
    └─ cv::imwrite → 保存 NG 图片
```

---

## cpp_wood_defect 独有的功能（TensorRT-YOLO 完全没有）

### 1. 海康工业相机 MVS SDK 直连

```
HikvisionCamera:
  - MV_CC_EnumDevices()         设备枚举
  - MV_CC_CreateHandle()        创建句柄
  - MV_CC_OpenDevice()          打开设备
  - MV_CC_StartGrabbing()       开始取流
  - BayerRG/GR/GB/BG → BGR      12种Bayer格式自动识别转换
  - 连续采集 / 软触发 / 硬触发   三种触发模式
  - 曝光时间 / 增益 动态设置
```

### 2. CLAHE 对比度增强（木板纹理场景特有）

```cpp
// 木板表面纹理复杂，CLAHE 能有效增强缺陷对比度
// TensorRT-YOLO 不做这个，在送入推理前手动应用
cv::Mat enhance(const cv::Mat& f, float clip, int tile) {
    cv::Mat lab;
    cv::cvtColor(f, lab, cv::COLOR_BGR2Lab);
    std::vector<cv::Mat> ch(3);
    cv::split(lab, ch);
    cv::createCLAHE(clip, cv::Size(tile, tile))->apply(ch[0], ch[0]);
    cv::merge(ch, lab);
    cv::cvtColor(lab, f, cv::COLOR_Lab2BGR);
    return f;
}
```

### 3. 规则引擎：瑕疵严重程度判定

```
crack       → 无条件 NG（裂纹不可接受）
hole        → 面积 > HOLE_MAX_AREA → NG，否则 WARN
knot        → 面积比 > 5% → NG，> 1% → WARN
scratch     → 长宽比 > 5x 且 长度 > 50px → NG
edge_damage → 无条件 NG
rot         → 无条件 NG
stain       → 面积比 > 3% → NG，否则 WARN
```

### 4. 产线集成

- 🔌 **GPIO 剔除信号**：检测到 NG → 通过 GPIO/串口 发信号给 PLC 执行剔除
- ⚙️ **硬触发模式**：流水线传感器信号 → Line0 → 相机自动拍照
- 📸 **瑕疵图片自动保存**：按时间戳 + NG/WARN 前缀自动存档
- 🏭 **产线速度参数**：可配置 `LINE_SPEED`

---

## TensorRT-YOLO 接管的部分（项目不再自己实现）

| 功能 | v1.0（自己写） | v2.0（TensorRT-YOLO 接管） |
|------|--------------|---------------------------|
| 引擎加载 | `runtime->deserializeCudaEngine()` | `DetectModel(engine_path, option)` |
| 推理执行 | `enqueueV2` | `enqueueV3`（库内部） |
| 预处理 | CPU resize + 归一化 + HWC→CHW | GPU letterbox kernel + swapRB |
| NMS | CPU 手写 IoU + 贪心抑制 | GPU EfficientNMS 插件 |
| CUDA Graph | ❌ 无 | ✅ 库内部自动 |
| 零拷贝 | ❌ 无 | ✅ 库内部自动检测 |
| 性能报告 | 简单 FPS 计数器 | ✅ 百分位延迟 + 吞吐量（可选） |

---

## ⚡ 光电传感器触发架构设计

### 需求确认

- 拍照后**必须对图像做 AI 处理**（检测木材瑕疵）
- 处理结果决定是否发剔除信号
- 处理在 NVIDIA Nano 上完成

### 结论：光电传感器 → Nano GPIO → 软件触发相机

```
光电传感器 ──(GPIO 中断)──▶ NVIDIA Nano ──(MVS SDK 软触发)──▶ 海康相机
                                                                    │
                                                              (图像回传)
                                                                    │
                                                                    ▼
                                                              TensorRT-YOLO 推理
                                                                    │
                                                                    ▼
                                                              规则引擎 → GPIO 剔除
```

### GPIO 接线参考（NVIDIA Jetson Nano）

```
光电传感器信号线 ──▶ 40-pin Header Pin 7 (GPIO 216)
光电传感器 GND   ──▶ Pin 6 (GND)

# 注意：Nano GPIO 为 3.3V 电平，如果传感器输出 5V/12V/24V，
# 需要加电平转换模块或光耦隔离
```

### 代码骨架

```cpp
// 1. GPIO 中断监听（libgpiod）
#include <gpiod.h>

// 2. 回调中执行软触发
void on_trigger() {
    MV_CC_SetEnumValue(handle, "TriggerMode", MV_TRIGGER_MODE_ON);
    MV_CC_SetEnumValue(handle, "TriggerSource", MV_TRIGGER_SOURCE_SOFTWARE);
    MV_CC_SetCommandValue(handle, "TriggerSoftware");
    MV_CC_GetOneFrameTimeout(handle, &stFrameInfo, 1000);

    // CLAHE 增强 + 推理 + 规则判定
    cv::Mat enhanced = enhance(frame, 2.0f, 8);
    auto res = infer.detect(enhanced);
    auto defects = post.process(res, enhanced, frame_size);

    // NG → 发剔除信号
    for (auto& d : defects)
        if (d.level == "ng") { gpio_set(REJECT_PIN, 1); break; }
}
```

---

## 编译（Jetson Nano）

### 环境检查

JetPack 已自带以下依赖，无需额外安装：

```bash
# 确认 TensorRT
dpkg -l | grep tensorrt
ls /usr/src/tensorrt/include/NvInfer.h

# 确认 CUDA
nvcc --version          # 应显示 CUDA 10.x
ls /usr/local/cuda

# 确认 OpenCV（JetPack 自带 CUDA 加速版）
python3 -c "import cv2; print(cv2.__version__)"
```

仅需额外安装 **海康 MVS SDK**（从海康官网下载 aarch64 版本）。

### 关键：CUDA 架构

Jetson Nano 是 **Maxwell 架构 (SM 5.3)**。TensorRT-YOLO 默认只编译 SM 72-90（桌面卡），
在 Nano 上编译必须指定 `-DCMAKE_CUDA_ARCHITECTURES=53`，否则编译出来的 `.so` 无法运行。

```bash
# 确认 Nano 的 GPU 架构
cd /usr/local/cuda/samples/1_Utilities/deviceQuery
sudo make
./deviceQuery | grep "CUDA Capability"
# 输出: CUDA Capability Major/Minor version number: 5.3
```

### 编译顺序

```bash
# ① 编译安装 TensorRT-YOLO 库
cd ~/TensorRT-YOLO
mkdir build && cd build
cmake -DTRT_PATH=/usr/src/tensorrt \
      -DCMAKE_CUDA_ARCHITECTURES=53 \
      ..
make -j4
sudo make install   # → /usr/local/lib/libtrtyolo.so + /usr/local/include/trtyolo/

# ② 编译本项目
cd ~/cpp_wood_defect
mkdir build && cd build
cmake -DTRTYOLO_PATH=/usr/local ..
make -j4
./wood_defect_detector
```

### Nano 上的性能优势

Jetson Nano 是 CPU+GPU **共享内存**的集成架构（iGPU），TensorRT-YOLO 会自动检测并走 **zero-copy** 路径，
省掉 Host↔Device 的 `cudaMemcpy`。

| 环节 | v1.0（手写） | v2.0（TensorRT-YOLO 库） |
|------|------------|-------------------------|
| 预处理 | CPU resize + 归一化 | **GPU letterbox kernel** |
| 推理 | enqueueV2 | **enqueueV3 + CUDA Graph** |
| NMS | CPU 手写贪心 | **GPU EfficientNMS** |
| 内存 | cudaMemcpy H→D→H | **zero-copy（自动检测 iGPU）** |
| 引擎构建 | ONNX→TRT（代码内置） | 预构建 .trt 文件直接加载 |
