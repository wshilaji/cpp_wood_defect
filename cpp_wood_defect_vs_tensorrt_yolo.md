# cpp_wood_defect vs TensorRT-YOLO 对比分析

## 概述

| 项目 | 定位 | 作者 |
|------|------|------|
| `cpp_wood_defect/` | 产线端到端木板瑕疵检测系统 | 定制开发 |
| `TensorRT-YOLO/` | 通用 YOLO 推理库，支持多任务 | laugh12321（开源） |

---

## 🔄 相似的部分（TensorRT API 标准用法）

这些相似是正常的——TensorRT C++ API 就那么一套流程，就像所有 HTTP 客户端都要 `connect()` → `send()` → `recv()` 一样。

### 1. TensorRT 引擎管理

| 步骤 | `cpp_wood_defect`（`trt_engine.cpp`） | `TensorRT-YOLO`（`backend.cpp`） |
|------|--------------------------------------|--------------------------------|
| 创建 Builder | `nvinfer1::createInferBuilder()` | 封装在 `TRTManager` 内部 |
| 解析 ONNX | `nvonnxparser::createParser()` → `parser->parse()` | 预构建 `.trt` 文件，不包含 ONNX 解析 |
| 设置 FP16 | `config->setFlag(BuilderFlag::kFP16)` | 用户通过 `InferOption` 间接配置 |
| 序列化引擎 | `engine->serialize()` → 写 `.trt` 文件 | `TRTManager` 内部处理 |
| 加载引擎 | `runtime->deserializeCudaEngine()` | `TRTManager::initialize()` |
| 执行上下文 | `engine->createExecutionContext()` | `TRTManager` 内部管理 |

### 2. CUDA 内存管理

```cpp
// 两边都是标准 CUDA API
cudaMalloc(&gpu_buf, size);          // 分配 GPU 内存
cudaMallocHost(&cpu_buf, size);      // 分配 CPU 固定内存（加速传输）
cudaMemcpyAsync(dst, src, size, ...); // 异步拷贝
cudaStreamCreate(&stream);           // 创建 CUDA 流
cudaStreamSynchronize(stream);       // 同步等待
```

### 3. 推理执行

```cpp
// cpp_wood_defect: V2 API
_context->enqueueV2(gpu_buffers, _stream, nullptr);

// TensorRT-YOLO: V3 API（更新）
manager_->enqueueV3(stream);
```

### 4. 图像预处理

| | `cpp_wood_defect`（CPU） | `TensorRT-YOLO`（GPU） |
|---|---|---|
| 缩放 | `cv::resize()` | `cudaLetterbox()` kernel |
| 色彩转换 | `cv::cvtColor(BGR→RGB)` | 通过 `enableSwapRB()` |
| 归一化 | `convertTo(CV_32FC3, 1.0/255.0)` → HWC→CHW | `alpha/beta` 参数化的 GPU kernel |
| 增强 | **CLAHE（LAB 色彩空间 L 通道）** | 无 |

### 5. YOLO 输出解码 + NMS

```cpp
// 两边的检测输出结构都是 YOLO 风格:
// [cx, cy, w, h, obj_conf, class_scores...]

// cpp_wood_defect: CPU 解码 + 手写 NMS
float* det = output + i * stride_size;
float cx = det[0], cy = det[1], w = det[2], h = det[3];
float obj_conf = det[4];
// → 遍历类别找 max → 阈值过滤 → 手写 IoU + 贪心 NMS

// TensorRT-YOLO: GPU EfficientNMS 插件（无需手写 NMS）
// 输出直接是: num, boxes, scores, classes
```

---

## 🔀 本质区别

### 架构设计

| 维度 | `cpp_wood_defect` | `TensorRT-YOLO` |
|------|------------------|-----------------|
| **设计模式** | 平铺直叙，易读易改 | Pimpl 惯用法（隐藏实现细节） |
| **API 风格** | 面向特定产线场景 | 通用库级 API，支持 Clone |
| **命名空间** | 无（全局作用域） | `trtyolo::` |
| **类层次** | 独立类：`TensorRTEngine`, `Preprocessor`, `Postprocessor` | 基类 `BaseModel` → `DetectModel`, `ClassifyModel`, `SegmentModel`, `OBBModel`, `PoseModel` |
| **配置方式** | `Config` 命名空间 + `constexpr` 常量 | `InferOption` 类（运行时配置） |

### 支持的任务类型

| | `cpp_wood_defect` | `TensorRT-YOLO` |
|---|---|---|
| 目标检测 (Detect) | ✅（唯一任务） | ✅ |
| 分类 (Classify) | ❌ | ✅ |
| 分割 (Segment) | ❌ | ✅ |
| 姿态估计 (Pose) | ❌ | ✅ |
| 旋转框检测 (OBB) | ❌ | ✅ |

### NMS 实现

| | `cpp_wood_defect` | `TensorRT-YOLO` |
|---|---|---|
| **位置** | CPU 端 C++ 手写 | GPU 端 CUDA kernel（EfficientNMSPlugin） |
| **实现** | 手写 IoU 计算 + 贪心抑制 | CUDA 优化并行实现 |
| **性能** | 检测目标少时足够 | 目标密集时显著更快 |
| **代码量** | ~50 行 | ~500+ 行 CUDA |

### 高级特性

| 特性 | `cpp_wood_defect` | `TensorRT-YOLO` |
|------|:-:|:-:|
| CUDA Graph 加速 | ❌ | ✅ |
| Zero-Copy（集成 GPU） | ❌ | ✅ |
| Unified Memory | ❌ | ✅ |
| 动态 Batch | ❌（固定 batch=1） | ✅ |
| 模型 Clone（多线程） | ❌ | ✅ |
| 性能报告（吞吐量/延迟） | 简单的 FPS 计数器 | ✅ 百分位延迟 + 吞吐量 |
| Batch 批量推理 | ❌ | ✅ |

---

## 🔌 `cpp_wood_defect` 独有的功能（TensorRT-YOLO 完全没有）

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

### 2. CLAHE 对比度增强（木材纹理场景特有）

```cpp
// 木板表面纹理复杂，CLAHE 能有效增强缺陷对比度
cv::cvtColor(resized, lab, cv::COLOR_BGR2Lab);  // BGR → LAB
cv::split(lab, lab_channels);
_clahe->apply(lab_channels[0], lab_channels[0]);  // 仅增强 L 通道
cv::merge(lab_channels, lab);
cv::cvtColor(lab, enhanced, cv::COLOR_Lab2BGR);  // LAB → BGR
```

### 3. 规则引擎：瑕疵严重程度判定

```cpp
crack       → 无条件 NG（裂纹不可接受）
hole        → 面积 > HOLE_MAX_AREA → NG，否则 WARN
knot        → 面积比 > 5% → NG，> 1% → WARN
scratch     → 长宽比 > 5x 且 长度 > 50px → NG
edge_damage → 无条件 NG
rot         → 无条件 NG
stain       → 面积比 > 3% → NG，否则 WARN
```

### 4. 产线集成

- 🔌 **GPIO 剔除信号**：检测到 NG → 通过 GPIO/串口 发信号给 PLC 执行剔除（预留接口）
- ⚙️ **硬触发模式**：流水线传感器信号 → Line0 → 相机自动拍照
- 📸 **瑕疵图片自动保存**：按时间戳 + NG/WARN 前缀自动存档
- 🏭 **产线速度参数**：可配置 `LINE_SPEED`

### 5. 工业相机调试功能

- CLI 命令行参数（`--serial`, `--ip`, `--trigger`, `--exposure`, `--gain`, `--list`）
- 一键枚举在线设备
- 运行时可调曝光/增益

---

## 📊 总结对比表

```
┌──────────────────────┬─────────────────────────┬──────────────────────────┐
│                      │    cpp_wood_defect       │      TensorRT-YOLO       │
├──────────────────────┼─────────────────────────┼──────────────────────────┤
│ 定位                 │ 工业产线检测产品          │ 通用 YOLO 推理 SDK       │
│ 引擎初始化骨架       │ 相似（TensorRT 标准API）  │ 相似                      │
│ 推理方式             │ enqueueV2                │ enqueueV3                │
│ NMS 实现             │ CPU 手写                 │ GPU CUDA kernel          │
│ 图像预处理           │ CPU OpenCV + CLAHE       │ GPU CUDA letterbox       │
│ 相机采集             │ ✅ 海康 MVS SDK           │ ❌ 无                     │
│ 瑕疵判定规则引擎     │ ✅ 多级判定               │ ❌ 无                     │
│ 产线集成             │ ✅ GPIO/触发/PLC          │ ❌ 无                     │
│ CUDA Graph            │ ❌                       │ ✅                       │
│ 动态 Batch            │ ❌                       │ ✅                       │
│ 代码复杂度           │ 较简单，适合定制          │ 较复杂，适合复用          │
│ 学习门槛             │ 低                       │ 高                       │
└──────────────────────┴─────────────────────────┴──────────────────────────┘
```

## 🎯 学习建议

1. **先读懂 `cpp_wood_defect`**：代码平铺直叙，先把 TensorRT 引擎创建、ONNX 解析、buffer 分配、推理执行这一套流程吃透。

2. **再读 `TensorRT-YOLO` 的进阶特性**：
   - `backend.cpp` → 学习 CUDA Graph 如何加速静态模型
   - `letterbox.cu` → 学习 GPU 端预处理 kernel
   - `EfficientNMSPlugin` → 学习 GPU 端 NMS kernel 写法
   - `Pimpl 惯用法` → 学习库级 API 设计模式

3. **可以搬过来用的**：
   - CUDA Graph 加速（你的模型输入固定，很适合）
   - GPU letterbox 替代 CPU resize（释放 CPU 做别的事）
   - `enqueueV3` 替代 `enqueueV2`（新版 API）

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
                                                              Nano 推理处理
```

**核心原因：** Nano 无论如何都要拿到图像和触发时刻的信息，与其分两路信号再同步，不如让 Nano 做唯一的触发中枢。这样可以：

- 精确关联触发时刻和图像帧（同一进程内的时间戳）
- 避免"相机拍了但 Nano 不知道是哪张"的同步问题
- 方便做去抖（debounce）、延时、条件过滤等逻辑
- 代码全在一个进程里，没有多设备同步的坑

### 三种场景方案

#### 场景 1：当前项目适用 —— 光电 → Nano GPIO → MVS 软触发

```
光电传感器 OUTPUT ──▶ Nano GPIO Pin（如 pin 7 / GPIO 216）
                            │
                    libgpiod 监听中断
                            │
                    MV_CC_SetCommandValue("TriggerMode", "On")
                    MV_CC_SetCommandValue("TriggerSource", "Software")
                    MV_CC_SetCommandValue("TriggerSoftware")
                            │
                    MV_CC_GetOneFrameTimeout() 取图
                            │
                    TensorRT 推理 → 规则引擎 → GPIO 剔除信号
```

| 优点 | 缺点 |
|------|------|
| 所有控制逻辑在 Nano 内闭环 | 端到端延迟 ~5-30ms（软触发 + 曝光 + 取图） |
| 无需外部分线器 | 依赖 Nano 系统稳定运行 |
| 触发-图像帧精确对应 | 不适合 <1ms 级硬实时场景 |
| 可做复杂触发逻辑（去抖/条件过滤） | |

#### 场景 2：硬实时场景 —— 光电信号并给相机 + Nano

```
                     ┌──▶ 海康相机 Line0（硬触发，微秒级响应）
光电传感器 OUTPUT ──┤
                     └──▶ Nano GPIO 中断（记录时间戳、事件日志、后处理决策）
                            
相机拍摄 → MVS SDK 取帧 → Nano 推理处理
```

- 相机设置为 `TriggerSource = "Line0"`（外部硬触发）
- Nano 通过 GPIO 中断**仅做事件记录和取图后的判读**
- 拍照延迟可达微秒级

**仅当产线速度极快（例如 > 100 块板/分钟）且软触发延迟不可接受时才需要。**

#### 场景 3：混合方案（推荐留作扩展）

```
                     ┌──▶ 海康相机 Trigger In（硬触发拍照）
光电传感器 OUTPUT ──┤
                     └──▶ Nano GPIO 中断（记录时间戳、决定保留/丢弃当前帧）
                            
相机拍摄 → MVS SDK 取帧 → Nano 推理 → 规则引擎 → GPIO 剔除
```

Nano 在取到帧后还可以**根据时间戳判断是否丢弃**（比如连续触发时做跳帧），兼顾实时性与智能处理。

### 对比总结

| 维度 | Nano 中转触发 | 直接硬触发相机 |
|------|:-----------:|:-----------:|
| 拍照延迟 | ~5-30ms | <1ms |
| 图像与事件的精确关联 | ✅ 天然一致 | ⚠️ 需时间戳对齐 |
| 复杂触发逻辑（去抖/条件） | ✅ 灵活 | ❌ 受限 |
| 抗 Nano 宕机 | ❌ 无 | ✅ 独立运行 |
| 消费级/USB 相机可用 | ✅ | ❌（需硬触发口） |
| 代码复杂度 | 低 | 中（需两路信号同步） |
| **本项目适用** | ✅ **当前方案** | 仅高速产线需要 |

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
// 监听 GPIO 216 的上升沿，触发回调

// 2. 回调中执行软触发
void on_trigger() {
    // 设置触发模式为软触发
    MV_CC_SetEnumValue(handle, "TriggerMode", MV_TRIGGER_MODE_ON);
    MV_CC_SetEnumValue(handle, "TriggerSource", MV_TRIGGER_SOURCE_SOFTWARE);

    // 发软触发命令
    MV_CC_SetCommandValue(handle, "TriggerSoftware");

    // 取图
    MV_CC_GetOneFrameTimeout(handle, &stFrameInfo, 1000);

    // 推理 + 规则判定
    auto defects = infer(stFrameInfo.pBufAddr);
    auto verdict = rule_engine(defects);

    // NG → 发剔除信号
    if (verdict == NG) gpio_set(REJECT_PIN, 1);
}
```

> **注：** 当前 `cpp_wood_defect` 项目已包含 MVS SDK 相机采集和 GPIO 信号预留接口，上述代码为架构示意，具体实现参照项目中 `HikvisionCamera` 类和 GPIO 相关代码。
