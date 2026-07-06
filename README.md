# 木板瑕疵检测系统 — NVIDIA Jetson Nano + 海康工业相机

> C++17 · TensorRT FP16 · MVS SDK · 流水线实时目标检测

---

## 1. 系统架构

```
┌─────────────────────────────────────────────────────────────────────┐
│                         流水线现场                                    │
│                                                                     │
│   ┌──────────┐    触发信号     ┌──────────────┐                      │
│   │ 光电传感器 │ ────────────→ │ 海康工业相机   │                      │
│   │ (木板到位) │    Line0       │ (硬触发拍照)   │                      │
│   └──────────┘                └──────┬───────┘                      │
│                                      │ GigE/USB                     │
│                                      ▼                              │
│   ┌──────────────────────────────────────────────┐                  │
│   │            NVIDIA Jetson Nano                │                  │
│   │                                              │                  │
│   │  ┌──────────┐  ┌──────────┐  ┌──────────┐   │     ┌─────────┐  │
│   │  │ MVS SDK  │  │ CLAHE    │  │ TensorRT │   │     │ PLC/    │  │
│   │  │ 图像回调  │→ │ 预处理    │→ │ FP16推理  │──┼────→│ GPIO    │  │
│   │  │ B→BGR    │  │ 640×640  │  │ 瑕疵检测  │   │     │ 剔除信号 │  │
│   │  └──────────┘  └──────────┘  └──────────┘   │     └─────────┘  │
│   │                                              │                  │
│   │  后处理: NMS → 规则引擎判定(OK/WARN/NG)       │                  │
│   │  显示 + 保存NG图片                            │                  │
│   └──────────────────────────────────────────────┘                  │
└─────────────────────────────────────────────────────────────────────┘
```

### 数据流

```
海康相机(SDK回调) → Bayer→BGR转换 → 帧缓冲队列(3帧)
    → 非阻塞read() → 缩放640×640 → CLAHE增强(LAB/L通道)
    → HWC→CHW归一化 → TensorRT FP16推理
    → 模型输出解码 → 按类NMS → 规则引擎判定
    → 画框+标签 → 显示/保存NG图 → PLC剔除信号
```

### 耗时拆解（单帧）

| 步骤 | 典型耗时 | 运行位置 |
|------|---------|---------|
| 图像采集(回调Bayer→BGR) | 2-5ms | CPU |
| 预处理(缩放+CLAHE+CHW) | 2-5ms | CPU |
| TensorRT FP16 推理 | 15-30ms | **GPU** |
| 后处理(NMS+规则判定) | 1-3ms | CPU |
| **合计** | **~20-40ms (25-50 FPS)** | |

---

## 2. 目录结构

```
cpp_wood_defect/
├── CMakeLists.txt              # CMake构建(TensorRT + CUDA + OpenCV + MVS)
├── build.sh                    # 一键编译脚本
├── include/
│   ├── config.h                # 全局配置(相机/模型/阈值/类别)
│   ├── camera.h                # 海康MVS SDK相机采集
│   ├── preprocessor.h          # 图像预处理(缩放/CLAHE/归一化/CHW)
│   ├── trt_engine.h            # TensorRT推理引擎
│   └── postprocessor.h         # 后处理(NMS/规则判定/画框/保存)
├── src/
│   ├── main.cpp                # 主检测循环
│   ├── camera.cpp              # 海康相机实现
│   ├── preprocessor.cpp        # 预处理实现
│   ├── trt_engine.cpp          # TensorRT ONNX→TRT构建/加载/推理
│   └── postprocessor.cpp       # 后处理实现
└── models/                     # 放置 ONNX / TRT 模型文件
```

---

## 3. 各模块职责

### 3.1 `config.h` — 全局配置中心

所有可调参数集中在一个 namespace，按模块分组：

```
Config
├── 海康相机: 序列号/IP/UserID, 触发模式, 曝光, 增益, 分辨率
├── 模型: 输入尺寸, 置信度阈值, NMS阈值, ONNX/TRT路径, FP16开关
├── 预处理: CLAHE开关, clip限制, tile大小
├── 瑕疵类别: 8类 (背景/节疤/裂纹/孔洞/污渍/划痕/边角破损/腐朽)
├── 判定阈值: 各类别面积比/长宽比/长度阈值(OK/WARN/NG分界)
├── 产线参数: 线速度, 输出目录
└── 调试: 显示开关, FPS打印间隔
```

### 3.2 `camera.h/cpp` — 海康MVS SDK采集

**支持三种触发模式：**

| 模式 | 值 | 适用场景 | 行为 |
|------|---|---------|------|
| 连续采集 | 0 | 调试/测试 | 相机持续出图，回调收图 |
| 软触发 | 1 | 程序控制 | `softwareTrigger()` 发一次拍一张 |
| 硬触发 | 2 | 流水线 | Line0 等待光电传感器信号触发 |

**像素格式自动适配：**
- BayerRG8/GR8/GB8/BG8（自动 demosaicing → BGR）
- BayerRG10/GR10/GB10/BG10、BayerRG12/GR12/GB12/BG12
- RGB8_Packed → BGR 转换
- BGR8_Packed → 直接拷贝
- Mono8 → 灰度转BGR

**设备选型（优先级从高到低）：**
1. 序列号匹配 → `CAMERA_SERIAL`
2. IP 匹配 → `CAMERA_IP`
3. 自定义名称 → `CAMERA_USER_ID`
4. 以上全空 → 连接第一个在线设备

### 3.3 `preprocessor.h/cpp` — 图像预处理

```
输入BGR(HWC,uint8) → resize(640×640)
    → BGR→LAB, L通道CLAHE增强 → LAB→BGR     ← 对比度增强
    → BGR→RGB → /255归一化 → HWC→CHW → float* 输出
```

CLAHE 在 LAB 空间的 L 通道上做，避免颜色失真，有效增强木板纹理对比度。

### 3.4 `trt_engine.h/cpp` — TensorRT 推理

**首次运行自动构建：**
```
检测 models/defect_model.trt 是否存在
  ├── 存在 → 加载并反序列化
  └── 不存在 → 从 ONNX 构建(FP16) → 序列化保存
```

**优化措施：**
- FP16 推理（Jetson Nano GPU 有 Tensor Core，FP16 速度约为 FP32 的 2 倍）
- CUDA pinned memory（加速 CPU↔GPU 传输）
- CUDA stream 异步拷贝 + 异步推理
- Buffer 复用（避免反复分配）

### 3.5 `postprocessor.h/cpp` — 后处理与判定

**后处理流水线:**
```
模型输出(float[N][5+cls]) → 置信度过滤 → NMS(同类抑制)
    → 坐标缩放(输入尺寸→显示尺寸) → 规则引擎判定 → 画框 → 保存
```

**规则引擎分级判定示例：**

| 瑕疵 | OK | WARN | NG |
|------|-----|------|-----|
| 裂纹 | — | — | 任意大小 |
| 节疤 | <1%面积 | 1%-5%面积 | >5%面积 |
| 孔洞 | — | ≤100px² | >100px² |
| 划痕 | ≤30px 或 长宽比<5 | 30-50px,长宽比>5 | >50px,长宽比>5 |
| 边角破损 | — | — | 任意大小 |
| 腐朽 | — | — | 任意大小 |
| 污渍 | — | ≤3%面积 | >3%面积 |

### 3.6 `main.cpp` — 主检测循环

```
初始化:
  1. 海康相机: 按序列号/IP连接 → 设置触发模式/曝光/增益 → 注册回调 → 开始取流
  2. TensorRT: 检测.trt是否存在 → 加载/构建引擎 → 分配Buffer
  3. 预处理器: 创建CLAHE, 预分配tensor内存
  4. 后处理器: 加载类别名/阈值

主循环(while running):
  Step 1: camera.read() 非阻塞取帧
  Step 2: preprocessor.process() 预处理
  Step 3: engine.infer() TensorRT推理
  Step 4: postprocessor.process() 解码+NMS+判定+画框
  Step 5: NG时发送PLC剔除信号, 保存NG截图
  Step 6: 显示(FPS/状态叠加), 按Q退出
```

---

## 4. 快速开始

### 4.1 环境要求

| 组件 | 版本/说明 |
|------|---------|
| Jetson Nano | JetPack 4.6+ (L4T 32.x) |
| CUDA | 10.2 (JetPack自带) |
| TensorRT | 7.x/8.x (JetPack自带) |
| OpenCV | 4.x with CUDA (JetPack自带) |
| 海康MVS SDK | 2.x ARM版, 默认安装到 `/opt/MVS/` |
| cmake | ≥3.14 |

### 4.2 部署步骤

```bash
# 1. 安装海康MVS SDK
#    从海康官网下载 ARM64 版本，按文档安装到 /opt/MVS/

# 2. 把 ONNX 模型放到 models/ 目录
cp /path/to/your_model.onnx models/defect_model.onnx

# 3. 修改配置（相机序列号等）
vim include/config.h

# 4. 编译
./build.sh

# 5. 运行
./build/wood_defect_detector
```

### 4.3 关键配置项（`config.h`）

**流水线模式（硬触发）：**
```cpp
CAMERA_SERIAL        = "DA1234567";   // 填相机序列号
CAMERA_TRIGGER_MODE  = 2;            // 硬触发(Line0)
CAMERA_EXPOSURE_US   = 5000.0f;      // 曝光5ms（根据线速度调）
CAMERA_GAIN_DB       = 0.0f;         // 增益（光线不足时加大）
```

**调试模式（连续采集）：**
```cpp
CAMERA_TRIGGER_MODE  = 0;            // 连续模式
SHOW_DISPLAY         = true;         // 显示画面
PRINT_FPS            = true;         // 每100帧打FPS
SAVE_DEFECT_IMAGES   = false;        // 调试时关闭保存
```

**首次运行 ONNX→TRT 构建：**
```
程序会自动检测 models/defect_model.trt 是否存在，
不存在则从 ONNX 构建 FP16 引擎（首次约2-5分钟），
构建后自动保存，后续直接加载（秒级启动）。
```

---

## 5. 完整源代码

### 5.1 `CMakeLists.txt`

```cmake
cmake_minimum_required(VERSION 3.14)
project(wood_defect_detector VERSION 1.0.0 LANGUAGES CXX CUDA)

# ============================================================
# C++17 标准
# ============================================================
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CUDA_STANDARD 14)
set(CMAKE_CUDA_STANDARD_REQUIRED ON)

# ============================================================
# Jetson Nano 优化编译选项
# ============================================================
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -march=armv8.2-a -mtune=cortex-a57")
    set(CMAKE_CUDA_FLAGS "${CMAKE_CUDA_FLAGS} -O3")
endif()

# 启用 TensorRT 和 CUDA
find_package(CUDA REQUIRED)

# ============================================================
# TensorRT
# ============================================================
set(TENSORRT_PATH "/usr/src/tensorrt" CACHE PATH "TensorRT 安装路径")
find_library(TENSORRT_LIB nvinfer PATHS
    ${TENSORRT_PATH}/lib
    /usr/lib/aarch64-linux-gnu
    /usr/lib
    REQUIRED)
find_library(TENSORRT_PLUGIN_LIB nvinfer_plugin PATHS
    ${TENSORRT_PATH}/lib
    /usr/lib/aarch64-linux-gnu
    /usr/lib)
find_path(TENSORRT_INCLUDE NvInfer.h PATHS
    ${TENSORRT_PATH}/include
    /usr/include/aarch64-linux-gnu
    /usr/include
    REQUIRED)

# ============================================================
# 海康 MVS SDK
# ============================================================
set(MVS_PATH "/opt/MVS" CACHE PATH "海康 MVS SDK 安装路径")
find_library(MVS_LIB MvCameraControl PATHS
    ${MVS_PATH}/lib/aarch64
    ${MVS_PATH}/lib/armhf
    ${MVS_PATH}/lib/64
    ${MVS_PATH}/lib/32
    ${MVS_PATH}/lib
    /usr/lib
    REQUIRED)
find_path(MVS_INCLUDE MvCameraControl.h PATHS
    ${MVS_PATH}/include
    /usr/include
    REQUIRED)
message(STATUS "  MVS SDK:       ${MVS_LIB}")

# ============================================================
# OpenCV (with CUDA on Jetson)
# ============================================================
find_package(OpenCV REQUIRED)

# ============================================================
# CUDA Toolkit
# ============================================================
find_library(CUDA_LIB cuda PATHS /usr/local/cuda/lib64 /usr/local/cuda/lib REQUIRED)
find_library(CUDART_LIB cudart PATHS /usr/local/cuda/lib64 /usr/local/cuda/lib REQUIRED)
find_path(CUDA_INCLUDE cuda_runtime.h PATHS /usr/local/cuda/include REQUIRED)

# ============================================================
# 源文件
# ============================================================
set(SOURCES
    src/main.cpp
    src/camera.cpp
    src/preprocessor.cpp
    src/trt_engine.cpp
    src/postprocessor.cpp
)

set(HEADERS
    include/config.h
    include/camera.h
    include/preprocessor.h
    include/trt_engine.h
    include/postprocessor.h
)

# ============================================================
# 可执行文件
# ============================================================
add_executable(${PROJECT_NAME} ${SOURCES} ${HEADERS})

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${TENSORRT_INCLUDE}
    ${CUDA_INCLUDE}
    ${MVS_INCLUDE}
    ${OpenCV_INCLUDE_DIRS}
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    ${TENSORRT_LIB}
    ${TENSORRT_PLUGIN_LIB}
    ${CUDA_LIB}
    ${CUDART_LIB}
    ${MVS_LIB}
    ${OpenCV_LIBS}
    pthread
)

# ============================================================
# 打印配置信息
# ============================================================
message(STATUS "========================================")
message(STATUS "  Wood Defect Detector - Build Config")
message(STATUS "========================================")
message(STATUS "  Build Type:    ${CMAKE_BUILD_TYPE}")
message(STATUS "  TensorRT Lib:  ${TENSORRT_LIB}")
message(STATUS "  OpenCV:        ${OpenCV_VERSION}")
message(STATUS "  CUDA:          ${CUDA_VERSION}")
message(STATUS "========================================")
```

### 5.2 `include/config.h`

```cpp
#pragma once

#include <string>
#include <vector>
#include <cstdint>

// ============================================================
// 木板瑕疵检测系统 - 全局配置
// ============================================================
namespace Config {

// ==================== 海康工业相机参数 ====================
// --- 设备选型（三选一，优先级: Serial > IP > UserID）---
constexpr const char* CAMERA_SERIAL    = "";            // 相机序列号，如 "DA1234567"
constexpr const char* CAMERA_IP        = "192.168.1.10";
constexpr const char* CAMERA_USER_ID   = "";            // 用户自定义名称
// --- 触发模式 ---
//   CONTINUOUS = 连续采集（调试用）
//   SOFTWARE   = 软触发（程序发指令拍照）
//   HARDWARE   = 硬触发（流水线传感器信号触发 Line0）
constexpr int       CAMERA_TRIGGER_MODE = 2;            // 0=连续, 1=软触发, 2=硬触发
constexpr int       CAMERA_WIDTH        = 1920;
constexpr int       CAMERA_HEIGHT       = 1080;
constexpr float     CAMERA_EXPOSURE_US  = 5000.0f;      // 曝光时间（微秒）
constexpr float     CAMERA_GAIN_DB      = 0.0f;         // 模拟增益（dB）

// ==================== 模型参数 ====================
constexpr int   INPUT_WIDTH           = 640;
constexpr int   INPUT_HEIGHT          = 640;
constexpr int   INPUT_CHANNELS        = 3;
constexpr float CONF_THRESHOLD        = 0.5f;
constexpr float NMS_THRESHOLD         = 0.4f;

constexpr const char* MODEL_PATH      = "models/defect_model.trt";
constexpr const char* ONNX_PATH       = "models/defect_model.onnx";
constexpr bool  USE_FP16              = true;        // FP16推理（Jetson Nano强烈建议开启）
constexpr int   MAX_WORKSPACE_SIZE    = 1 << 28;     // 256MB

// ==================== 预处理参数 ====================
constexpr bool  ENABLE_CLAHE          = true;
constexpr float CLAHE_CLIP_LIMIT      = 2.0f;
constexpr int   CLAHE_TILE_SIZE       = 8;

// ==================== 瑕疵类别 ====================
const std::vector<std::string> CLASS_NAMES = {
    "background",
    "knot",         // 节疤
    "crack",        // 裂纹
    "hole",         // 虫眼/孔洞
    "stain",        // 污渍/色差
    "scratch",      // 划痕
    "edge_damage",  // 边角破损
    "rot",          // 腐朽
};

// ==================== 判定阈值 ====================
namespace Rule {
    constexpr float HOLE_MAX_AREA        = 100.0f;   // 孔洞面积阈值 (px²)
    constexpr float KNOT_NG_RATIO        = 0.05f;    // 节疤NG面积比
    constexpr float KNOT_WARN_RATIO      = 0.01f;    // 节疤Warning面积比
    constexpr float SCRATCH_NG_LENGTH    = 50.0f;    // 划痕NG长度 (px)
    constexpr float SCRATCH_WARN_LENGTH  = 30.0f;    // 划痕Warning长度 (px)
    constexpr float SCRATCH_ASPECT       = 5.0f;     // 划痕最小长宽比
    constexpr float STAIN_NG_RATIO       = 0.03f;    // 污渍NG面积比
}

// ==================== 产线参数 ====================
constexpr float LINE_SPEED             = 0.5f;       // m/s
constexpr const char* OUTPUT_DIR       = "./output/";
constexpr bool  SAVE_DEFECT_IMAGES     = true;

// ==================== 调试 ====================
constexpr bool  SHOW_DISPLAY           = true;       // 显示画面
constexpr bool  PRINT_FPS              = true;       // 打印FPS
constexpr int   FPS_LOG_INTERVAL       = 100;        // 每N帧打印一次FPS

} // namespace Config
```

### 5.3 `include/camera.h`

```cpp
#pragma once

#include <opencv2/opencv.hpp>
#include <string>
#include <atomic>
#include <thread>
#include <mutex>
#include <deque>
#include <condition_variable>

// 海康 MVS SDK 头文件
#include "MvCameraControl.h"

/**
 * 海康工业相机采集模块
 *
 * 使用海康 MVS SDK 直连相机，支持:
 *  - 连续采集模式（调试用）
 *  - 软触发模式（程序发指令拍照）
 *  - 硬触发模式（流水线传感器信号 → Line0 → 相机拍照）
 *
 * 图像通过 SDK 回调函数接收 → Bayer→BGR 转换 → 存入缓冲队列
 * 主线程非阻塞 read() 取最新帧
 */
class HikvisionCamera {
public:
    HikvisionCamera();
    ~HikvisionCamera();

    /**
     * 根据序列号查找并连接相机
     * @param serial 相机序列号，空字符串表示不使用
     * @param ip     相机 IP 地址
     * @param user_id 用户自定义名称，空字符串表示不使用
     */
    bool connectBySerial(const std::string& serial,
                         const std::string& ip = "",
                         const std::string& user_id = "");

    /**
     * 根据 IP 查找并连接相机
     */
    bool connectByIP(const std::string& ip);

    /**
     * 根据用户自定义名称查找并连接相机
     */
    bool connectByUserID(const std::string& user_id);

    /**
     * 配置采集参数并开始取流
     * @param width          图像宽度 (0 = 不修改)
     * @param height         图像高度 (0 = 不修改)
     * @param exposure_us    曝光时间（微秒），<0 = 不修改
     * @param gain_db        模拟增益（dB），<0 = 不修改
     * @param trigger_mode   0=连续, 1=软触发, 2=硬触发
     * @return 成功返回 true
     */
    bool start(int width, int height,
               float exposure_us = -1.0f,
               float gain_db = -1.0f,
               int trigger_mode = 2);

    /** 停止采集并关闭相机 */
    void stop();

    /** 非阻塞读取最新一帧，无新帧时返回空 Mat */
    cv::Mat read();

    /** 是否正在运行 */
    bool isRunning() const { return _running.load(); }

    /** 软触发: 发送一次拍照指令（仅 trigger_mode=1 时有效） */
    bool softwareTrigger();

    // ---- 参数读写 ----
    bool setExposureTime(float us);
    bool setGain(float db);
    bool setTriggerMode(int mode);
    float getExposureTime();
    float getGain();

private:
    // ---- 内部辅助 ----
    /** 枚举所有在线设备 */
    bool enumDevices();

    /** 在设备列表里按条件匹配设备 */
    int  findDevice(const std::string& serial,
                    const std::string& ip,
                    const std::string& user_id);

    /** 创建句柄并打开设备 */
    bool openDevice(int index);

    /** 设置像素格式 */
    bool setPixelFormat(const std::string& format);

    /** 关闭设备并销毁句柄 */
    void closeDevice();

    /** 注册图像回调 */
    bool registerCallback();

    // ---- 静态回调（SDK C 接口） ----
    static void __stdcall onImageCallback(unsigned char* pData,
                                          MV_FRAME_OUT_INFO_EX* pFrameInfo,
                                          void* pUser);

    /** 回调中执行的图像处理 */
    void handleImage(unsigned char* pData, MV_FRAME_OUT_INFO_EX* pFrameInfo);

    // ---- 像素格式转换 ----
    /** BayerRG8 → BGR (OpenCV 实现) */
    cv::Mat bayerToBGR(const cv::Mat& bayer);

    // ---- 成员变量 ----
    void*                       _handle = nullptr;      // MV_CC_CreateHandle 返回
    std::atomic<bool>           _running{false};
    std::atomic<bool>           _grabbing{false};

    MV_CC_DEVICE_INFO_LIST      _device_list;
    int                         _device_index = -1;

    // 当前像素格式
    unsigned int                _pixel_format = 0;     // MvGvspPixelType 枚举值
    bool                        _is_bayer = false;

    // 帧缓冲队列
    std::deque<cv::Mat>         _buffer;
    static constexpr size_t     _MAX_BUFFER = 3;
    std::mutex                  _mutex;
    std::condition_variable     _cv;
};
```

### 5.4 `src/camera.cpp`

```cpp
#include "camera.h"
#include <iostream>
#include <chrono>
#include <sstream>
#include <cstring>
#include <cmath>

// ============================================================
// 构造 / 析构
// ============================================================
HikvisionCamera::HikvisionCamera() {
    memset(&_device_list, 0, sizeof(_device_list));
}

HikvisionCamera::~HikvisionCamera() {
    stop();
}

// ============================================================
// 设备枚举
// ============================================================
bool HikvisionCamera::enumDevices() {
    memset(&_device_list, 0, sizeof(_device_list));

    // 枚举所有 GigE / USB / CameraLink 设备
    int ret = MV_CC_EnumDevices(MV_GIGE_DEVICE | MV_USB_DEVICE, &_device_list);
    if (ret != MV_OK) {
        std::cerr << "[Camera] 枚举设备失败, 错误码: 0x"
                  << std::hex << ret << std::dec << std::endl;
        return false;
    }

    if (_device_list.nDeviceNum == 0) {
        std::cerr << "[Camera] 未找到任何海康相机设备" << std::endl;
        return false;
    }

    std::cout << "[Camera] 找到 " << _device_list.nDeviceNum
              << " 个设备:" << std::endl;

    for (unsigned int i = 0; i < _device_list.nDeviceNum; ++i) {
        MV_CC_DEVICE_INFO& info = _device_list.pDeviceInfo[i];
        char serial[64] = {0};
        char model[64]  = {0};

        if (info.nTLayerType == MV_GIGE_DEVICE) {
            // GigE 相机
            snprintf(serial, sizeof(serial), "%s",
                     info.SpecialInfo.stGigEInfo.chSerialNumber);
            snprintf(model, sizeof(model), "%s",
                     info.SpecialInfo.stGigEInfo.chModelName);
            uint32_t ip = info.SpecialInfo.stGigEInfo.nCurrentIp;
            std::cout << "  [" << i << "] " << model
                      << " | SN: " << serial
                      << " | IP: " << ((ip >> 24) & 0xFF) << "."
                                    << ((ip >> 16) & 0xFF) << "."
                                    << ((ip >> 8) & 0xFF) << "."
                                    << (ip & 0xFF) << std::endl;
        } else if (info.nTLayerType == MV_USB_DEVICE) {
            // USB 相机
            snprintf(serial, sizeof(serial), "%s",
                     info.SpecialInfo.stUsb3VInfo.chSerialNumber);
            snprintf(model, sizeof(model), "%s",
                     info.SpecialInfo.stUsb3VInfo.chModelName);
            std::cout << "  [" << i << "] " << model
                      << " | SN: " << serial
                      << " | USB" << std::endl;
        }
    }
    return true;
}

// ============================================================
// 在设备列表中查找匹配设备
// ============================================================
int HikvisionCamera::findDevice(const std::string& serial,
                                 const std::string& ip,
                                 const std::string& user_id) {
    for (unsigned int i = 0; i < _device_list.nDeviceNum; ++i) {
        MV_CC_DEVICE_INFO& info = _device_list.pDeviceInfo[i];
        char dev_serial[64] = {0};
        char dev_user_id[64] = {0};
        char dev_ip_str[32] = {0};

        if (info.nTLayerType == MV_GIGE_DEVICE) {
            snprintf(dev_serial, sizeof(dev_serial), "%s",
                     info.SpecialInfo.stGigEInfo.chSerialNumber);
            snprintf(dev_user_id, sizeof(dev_user_id), "%s",
                     info.SpecialInfo.stGigEInfo.chUserDefinedName);
            uint32_t ip_val = info.SpecialInfo.stGigEInfo.nCurrentIp;
            snprintf(dev_ip_str, sizeof(dev_ip_str), "%u.%u.%u.%u",
                     (ip_val >> 24) & 0xFF, (ip_val >> 16) & 0xFF,
                     (ip_val >> 8) & 0xFF, ip_val & 0xFF);
        } else if (info.nTLayerType == MV_USB_DEVICE) {
            snprintf(dev_serial, sizeof(dev_serial), "%s",
                     info.SpecialInfo.stUsb3VInfo.chSerialNumber);
            snprintf(dev_user_id, sizeof(dev_user_id), "%s",
                     info.SpecialInfo.stUsb3VInfo.chUserDefinedName);
        }

        // 按序列号匹配（最高优先级）
        if (!serial.empty() && serial == dev_serial) {
            return static_cast<int>(i);
        }

        // 按 IP 匹配
        if (!ip.empty() && ip == dev_ip_str) {
            return static_cast<int>(i);
        }

        // 按用户自定义名称匹配
        if (!user_id.empty() && user_id == dev_user_id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// ============================================================
// 按条件连接相机
// ============================================================
bool HikvisionCamera::connectBySerial(const std::string& serial,
                                       const std::string& ip,
                                       const std::string& user_id) {
    if (!enumDevices()) return false;

    int idx = findDevice(serial, ip, user_id);
    if (idx < 0) {
        std::cerr << "[Camera] 未找到匹配的相机 (serial=" << serial
                  << ", ip=" << ip
                  << ", user_id=" << user_id << ")" << std::endl;
        return false;
    }
    return openDevice(idx);
}

bool HikvisionCamera::connectByIP(const std::string& ip) {
    if (!enumDevices()) return false;
    int idx = findDevice("", ip, "");
    if (idx < 0) {
        std::cerr << "[Camera] 未找到 IP=" << ip << " 的相机" << std::endl;
        return false;
    }
    return openDevice(idx);
}

bool HikvisionCamera::connectByUserID(const std::string& user_id) {
    if (!enumDevices()) return false;
    int idx = findDevice("", "", user_id);
    if (idx < 0) {
        std::cerr << "[Camera] 未找到 UserID=" << user_id << " 的相机" << std::endl;
        return false;
    }
    return openDevice(idx);
}

// ============================================================
// 打开设备
// ============================================================
bool HikvisionCamera::openDevice(int index) {
    _device_index = index;

    // 创建句柄
    int ret = MV_CC_CreateHandle(&_handle, _device_list.pDeviceInfo + index);
    if (ret != MV_OK) {
        std::cerr << "[Camera] 创建相机句柄失败, 错误码: 0x"
                  << std::hex << ret << std::dec << std::endl;
        return false;
    }

    // 打开设备
    ret = MV_CC_OpenDevice(_handle);
    if (ret != MV_OK) {
        std::cerr << "[Camera] 打开相机失败, 错误码: 0x"
                  << std::hex << ret << std::dec << std::endl;
        MV_CC_DestroyHandle(_handle);
        _handle = nullptr;
        return false;
    }

    std::cout << "[Camera] 相机已打开 (index=" << index << ")" << std::endl;

    // 获取当前像素格式
    MVCC_ENUMVALUE pixel_fmt;
    ret = MV_CC_GetEnumValue(_handle, "PixelFormat", &pixel_fmt);
    if (ret == MV_OK) {
        _pixel_format = pixel_fmt.nCurValue;
        std::cout << "[Camera] 当前像素格式: 0x"
                  << std::hex << _pixel_format << std::dec << std::endl;

        // 判断是否是 Bayer 格式
        unsigned int bayer_fmts[] = {
            PixelType_Gvsp_BayerGR8, PixelType_Gvsp_BayerRG8,
            PixelType_Gvsp_BayerGB8, PixelType_Gvsp_BayerBG8,
            PixelType_Gvsp_BayerGR10, PixelType_Gvsp_BayerRG10,
            PixelType_Gvsp_BayerGB10, PixelType_Gvsp_BayerBG10,
            PixelType_Gvsp_BayerGR12, PixelType_Gvsp_BayerRG12,
            PixelType_Gvsp_BayerGB12, PixelType_Gvsp_BayerBG12,
        };
        for (unsigned int fmt : bayer_fmts) {
            if (_pixel_format == fmt) {
                _is_bayer = true;
                break;
            }
        }
    }

    return true;
}

// ============================================================
// 关闭设备
// ============================================================
void HikvisionCamera::closeDevice() {
    if (_grabbing.load()) {
        MV_CC_StopGrabbing(_handle);
        _grabbing.store(false);
    }
    if (_handle) {
        MV_CC_CloseDevice(_handle);
        MV_CC_DestroyHandle(_handle);
        _handle = nullptr;
        std::cout << "[Camera] 相机已关闭" << std::endl;
    }
}

// ============================================================
// 设置像素格式
// ============================================================
bool HikvisionCamera::setPixelFormat(const std::string& format) {
    if (!_handle) return false;
    int ret = MV_CC_SetEnumValue(_handle, "PixelFormat",
                                  const_cast<char*>(format.c_str()));
    if (ret != MV_OK) {
        std::cerr << "[Camera] 设置像素格式 " << format
                  << " 失败, 错误码: 0x"
                  << std::hex << ret << std::dec << std::endl;
        return false;
    }
    std::cout << "[Camera] 像素格式设为: " << format << std::endl;
    return true;
}

// ============================================================
// 设置触发模式
// ============================================================
bool HikvisionCamera::setTriggerMode(int mode) {
    if (!_handle) return false;

    switch (mode) {
        case 0: {
            // 连续采集
            MV_CC_SetEnumValue(_handle, "TriggerMode",
                                (void*)(uintptr_t)MV_TRIGGER_MODE_OFF);
            break;
        }
        case 1: {
            // 软触发
            MV_CC_SetEnumValue(_handle, "TriggerMode",
                                (void*)(uintptr_t)MV_TRIGGER_MODE_ON);
            MV_CC_SetEnumValue(_handle, "TriggerSource",
                                (void*)(uintptr_t)MV_TRIGGER_SOURCE_SOFTWARE);
            break;
        }
        case 2: {
            // 硬触发 (Line0 接收外部传感器信号)
            MV_CC_SetEnumValue(_handle, "TriggerMode",
                                (void*)(uintptr_t)MV_TRIGGER_MODE_ON);
            MV_CC_SetEnumValue(_handle, "TriggerSource",
                                (void*)(uintptr_t)MV_TRIGGER_SOURCE_LINE0);
            // 设置触发极性: 上升沿触发
            MV_CC_SetEnumValue(_handle, "TriggerActivation",
                                (void*)(uintptr_t)MV_TRIGGER_ACTIVATION_RISING_EDGE);
            break;
        }
        default:
            std::cerr << "[Camera] 无效的触发模式: " << mode << std::endl;
            return false;
    }

    std::cout << "[Camera] 触发模式: "
              << (mode == 0 ? "连续" : mode == 1 ? "软触发" : "硬触发(Line0)")
              << std::endl;
    return true;
}

// ============================================================
// 设置曝光时间
// ============================================================
bool HikvisionCamera::setExposureTime(float us) {
    if (!_handle || us < 0) return false;
    int ret = MV_CC_SetFloatValue(_handle, "ExposureTime", us);
    if (ret != MV_OK) {
        std::cerr << "[Camera] 设置曝光失败, 错误码: 0x"
                  << std::hex << ret << std::dec << std::endl;
        return false;
    }
    std::cout << "[Camera] 曝光时间: " << us << " us" << std::endl;
    return true;
}

// ============================================================
// 设置增益
// ============================================================
bool HikvisionCamera::setGain(float db) {
    if (!_handle || db < 0) return false;
    int ret = MV_CC_SetFloatValue(_handle, "Gain", db);
    if (ret != MV_OK) {
        std::cerr << "[Camera] 设置增益失败, 错误码: 0x"
                  << std::hex << ret << std::dec << std::endl;
        return false;
    }
    std::cout << "[Camera] 模拟增益: " << db << " dB" << std::endl;
    return true;
}

// ============================================================
// 获取曝光
// ============================================================
float HikvisionCamera::getExposureTime() {
    if (!_handle) return -1.0f;
    MVCC_FLOATVALUE val;
    if (MV_CC_GetFloatValue(_handle, "ExposureTime", &val) == MV_OK) {
        return val.fCurValue;
    }
    return -1.0f;
}

// ============================================================
// 获取增益
// ============================================================
float HikvisionCamera::getGain() {
    if (!_handle) return -1.0f;
    MVCC_FLOATVALUE val;
    if (MV_CC_GetFloatValue(_handle, "Gain", &val) == MV_OK) {
        return val.fCurValue;
    }
    return -1.0f;
}

// ============================================================
// 软触发：发送一次拍照指令
// ============================================================
bool HikvisionCamera::softwareTrigger() {
    if (!_handle) return false;
    int ret = MV_CC_SetCommandValue(_handle, "TriggerSoftware");
    return (ret == MV_OK);
}

// ============================================================
// 注册图像回调
// ============================================================
bool HikvisionCamera::registerCallback() {
    if (!_handle) return false;
    int ret = MV_CC_RegisterImageCallBackEx(_handle, onImageCallback, this);
    if (ret != MV_OK) {
        std::cerr << "[Camera] 注册图像回调失败, 错误码: 0x"
                  << std::hex << ret << std::dec << std::endl;
        return false;
    }
    std::cout << "[Camera] 图像回调已注册" << std::endl;
    return true;
}

// ============================================================
// 启动采集
// ============================================================
bool HikvisionCamera::start(int width, int height,
                             float exposure_us,
                             float gain_db,
                             int trigger_mode) {
    if (!_handle) {
        std::cerr << "[Camera] 相机未打开，无法启动" << std::endl;
        return false;
    }

    // ---- 1. 触发模式 ----
    setTriggerMode(trigger_mode);

    // ---- 2. 曝光时间 ----
    if (exposure_us >= 0) setExposureTime(exposure_us);

    // ---- 3. 增益 ----
    if (gain_db >= 0) setGain(gain_db);

    // ---- 4. 图像尺寸（如果需要修改） ----
    if (width > 0 && height > 0) {
        MV_CC_SetIntValue(_handle, "Width",  width);
        MV_CC_SetIntValue(_handle, "Height", height);
    }

    // ---- 5. 注册回调 ----
    if (!registerCallback()) return false;

    // ---- 6. 开始取流 ----
    int ret = MV_CC_StartGrabbing(_handle);
    if (ret != MV_OK) {
        std::cerr << "[Camera] 开始取流失败, 错误码: 0x"
                  << std::hex << ret << std::dec << std::endl;
        return false;
    }

    _grabbing.store(true);
    _running.store(true);

    std::cout << "[Camera] 取流已启动 "
              << "(宽=" << width << ", 高=" << height
              << ", 曝光=" << exposure_us << "us"
              << ", 增益=" << gain_db << "dB)"
              << std::endl;

    return true;
}

// ============================================================
// 停止采集
// ============================================================
void HikvisionCamera::stop() {
    _running.store(false);
    closeDevice();

    // 清空缓冲
    std::lock_guard<std::mutex> lock(_mutex);
    _buffer.clear();

    std::cout << "[Camera] 已停止" << std::endl;
}

// ============================================================
// 读取最新帧（非阻塞）
// ============================================================
cv::Mat HikvisionCamera::read() {
    std::lock_guard<std::mutex> lock(_mutex);
    if (_buffer.empty()) {
        return cv::Mat();
    }
    return _buffer.back().clone();
}

// ============================================================
// SDK 图像回调入口 (static → handleImage)
// ============================================================
void __stdcall HikvisionCamera::onImageCallback(unsigned char* pData,
                                                  MV_FRAME_OUT_INFO_EX* pFrameInfo,
                                                  void* pUser) {
    if (pUser) {
        auto* self = static_cast<HikvisionCamera*>(pUser);
        self->handleImage(pData, pFrameInfo);
    }
}

// ============================================================
// 回调中处理图像: Bayer→BGR → 写入缓冲队列
// ============================================================
void HikvisionCamera::handleImage(unsigned char* pData,
                                   MV_FRAME_OUT_INFO_EX* pFrameInfo) {
    if (!pData || !pFrameInfo) return;

    int w = pFrameInfo->nWidth;
    int h = pFrameInfo->nHeight;

    cv::Mat bgr;

    if (_is_bayer) {
        // Bayer 格式 → BGR
        cv::Mat raw(h, w, CV_8UC1, pData);
        bgr = bayerToBGR(raw);
    } else if (_pixel_format == PixelType_Gvsp_RGB8_Packed) {
        // RGB → BGR
        cv::Mat rgb(h, w, CV_8UC3, pData);
        cv::cvtColor(rgb, bgr, cv::COLOR_RGB2BGR);
    } else if (_pixel_format == PixelType_Gvsp_BGR8_Packed) {
        // 直接 BGR，仅拷贝
        bgr = cv::Mat(h, w, CV_8UC3, pData).clone();
    } else if (_pixel_format == PixelType_Gvsp_Mono8) {
        // 灰度 → BGR
        cv::Mat gray(h, w, CV_8UC1, pData);
        cv::cvtColor(gray, bgr, cv::COLOR_GRAY2BGR);
    } else {
        // 未知格式，尝试当作 8-bit 单通道处理
        cv::Mat unknown(h, w, CV_8UC1, pData);
        cv::cvtColor(unknown, bgr, cv::COLOR_GRAY2BGR);
    }

    if (!bgr.empty()) {
        std::lock_guard<std::mutex> lock(_mutex);
        _buffer.push_back(bgr);
        while (_buffer.size() > _MAX_BUFFER) {
            _buffer.pop_front();
        }
    }
}

// ============================================================
// BayerRG8 → BGR
// 使用 OpenCV demosaicing
// ============================================================
cv::Mat HikvisionCamera::bayerToBGR(const cv::Mat& bayer) {
    cv::Mat bgr;
    // bayer 格式对应的 OpenCV 转换码
    static const std::map<unsigned int, int> BAYER_CODES = {
        {PixelType_Gvsp_BayerRG8,  cv::COLOR_BayerRG2BGR},
        {PixelType_Gvsp_BayerGR8,  cv::COLOR_BayerGR2BGR},
        {PixelType_Gvsp_BayerGB8,  cv::COLOR_BayerGB2BGR},
        {PixelType_Gvsp_BayerBG8,  cv::COLOR_BayerBG2BGR},
        {PixelType_Gvsp_BayerRG10, cv::COLOR_BayerRG2BGR},
        {PixelType_Gvsp_BayerGR10, cv::COLOR_BayerGR2BGR},
        {PixelType_Gvsp_BayerGB10, cv::COLOR_BayerGB2BGR},
        {PixelType_Gvsp_BayerBG10, cv::COLOR_BayerBG2BGR},
        {PixelType_Gvsp_BayerRG12, cv::COLOR_BayerRG2BGR},
        {PixelType_Gvsp_BayerGR12, cv::COLOR_BayerGR2BGR},
        {PixelType_Gvsp_BayerGB12, cv::COLOR_BayerGB2BGR},
        {PixelType_Gvsp_BayerBG12, cv::COLOR_BayerBG2BGR},
    };

    auto it = BAYER_CODES.find(_pixel_format);
    if (it != BAYER_CODES.end()) {
        cv::cvtColor(bayer, bgr, it->second);
    } else {
        // 默认 BayerRG
        cv::cvtColor(bayer, bgr, cv::COLOR_BayerRG2BGR);
    }

    return bgr;
}
```

### 5.5 `include/preprocessor.h`

```cpp
#pragma once

#include <opencv2/opencv.hpp>
#include <vector>
#include <array>

/**
 * 图像预处理模块
 *
 * 流水线: 缩放 → CLAHE增强 → 归一化 → 转CHW → 添加batch维度
 * 输出可直接送入 TensorRT 引擎
 */
class Preprocessor {
public:
    /**
     * @param target_w     模型输入宽度
     * @param target_h     模型输入高度
     * @param enable_clahe 是否启用 CLAHE 对比度增强
     * @param clip_limit   CLAHE 裁剪限制
     * @param tile_size    CLAHE 网格大小
     */
    Preprocessor(int target_w = 640, int target_h = 640,
                 bool enable_clahe = true,
                 float clip_limit = 2.0f, int tile_size = 8);

    /**
     * 执行预处理
     * @param frame    输入帧 (BGR, HWC, uint8)
     * @param out_tensor 输出张量 (float*, 已分配内存, 大小 = 1*C*H*W)
     * @param out_display 输出处理后的可视图像 (用于画框展示)
     */
    void process(const cv::Mat& frame, float* out_tensor, cv::Mat& out_display);

    /** 仅获取归一化后的显示用图（不分配tensor内存） */
    cv::Mat processForDisplay(const cv::Mat& frame);

    int targetWidth()  const { return _target_w; }
    int targetHeight() const { return _target_h; }
    int channels()     const { return _channels; }

private:
    int     _target_w;
    int     _target_h;
    int     _channels;
    bool    _enable_clahe;
    float   _clip_limit;
    int     _tile_size;

    cv::Ptr<cv::CLAHE> _clahe;
};
```

### 5.6 `src/preprocessor.cpp`

```cpp
#include "preprocessor.h"
#include <iostream>

Preprocessor::Preprocessor(int target_w, int target_h,
                           bool enable_clahe,
                           float clip_limit, int tile_size)
    : _target_w(target_w)
    , _target_h(target_h)
    , _channels(3)
    , _enable_clahe(enable_clahe)
    , _clip_limit(clip_limit)
    , _tile_size(tile_size)
{
    if (_enable_clahe) {
        _clahe = cv::createCLAHE(_clip_limit,
                                  cv::Size(_tile_size, _tile_size));
    }
}

void Preprocessor::process(const cv::Mat& frame,
                            float* out_tensor,
                            cv::Mat& out_display)
{
    // ----------------------------------------------------------
    // Step 1: 降采样到模型输入分辨率
    // ----------------------------------------------------------
    cv::Mat resized;
    cv::resize(frame, resized, cv::Size(_target_w, _target_h),
               0, 0, cv::INTER_LINEAR);

    // ----------------------------------------------------------
    // Step 2: CLAHE 增强（在 LAB 色彩空间的 L 通道上）
    // ----------------------------------------------------------
    cv::Mat enhanced;
    if (_enable_clahe && _clahe) {
        cv::Mat lab;
        cv::cvtColor(resized, lab, cv::COLOR_BGR2Lab);

        // 分离通道
        std::vector<cv::Mat> lab_channels(3);
        cv::split(lab, lab_channels);

        // 对 L 通道做 CLAHE
        _clahe->apply(lab_channels[0], lab_channels[0]);

        // 合并
        cv::merge(lab_channels, lab);
        cv::cvtColor(lab, enhanced, cv::COLOR_Lab2BGR);
    } else {
        enhanced = resized;
    }

    // 保存增强后的图像给外部用于显示
    out_display = enhanced.clone();

    // ----------------------------------------------------------
    // Step 3: BGR → RGB, 归一化到 [0, 1], HWC → CHW
    // ----------------------------------------------------------
    cv::Mat rgb;
    cv::cvtColor(enhanced, rgb, cv::COLOR_BGR2RGB);

    // 转换为 float 并归一化
    rgb.convertTo(rgb, CV_32FC3, 1.0 / 255.0);

    // HWC → CHW: 直接逐通道写入输出 buffer
    const int plane_size = _target_w * _target_h;
    const float* data = reinterpret_cast<float*>(rgb.data);

    for (int h = 0; h < _target_h; ++h) {
        for (int w = 0; w < _target_w; ++w) {
            int hwc_idx = (h * _target_w + w) * 3; // HWC index
            for (int c = 0; c < 3; ++c) {
                out_tensor[c * plane_size + h * _target_w + w] = data[hwc_idx + c];
            }
        }
    }
}

cv::Mat Preprocessor::processForDisplay(const cv::Mat& frame) {
    cv::Mat resized;
    cv::resize(frame, resized, cv::Size(_target_w, _target_h),
               0, 0, cv::INTER_LINEAR);

    if (_enable_clahe && _clahe) {
        cv::Mat lab;
        cv::cvtColor(resized, lab, cv::COLOR_BGR2Lab);
        std::vector<cv::Mat> lab_channels(3);
        cv::split(lab, lab_channels);
        _clahe->apply(lab_channels[0], lab_channels[0]);
        cv::merge(lab_channels, lab);
        cv::cvtColor(lab, resized, cv::COLOR_Lab2BGR);
    }

    return resized;
}
```

### 5.7 `include/trt_engine.h`

```cpp
#pragma once

#include <NvInfer.h>
#include <cuda_runtime.h>

#include <string>
#include <vector>
#include <memory>
#include <cstdint>

/**
 * TensorRT 推理引擎
 *
 * 支持:
 *  - 从 .trt 文件加载预构建引擎
 *  - 从 .onnx 文件构建引擎（首次运行）
 *  - FP16 推理（Jetson Nano 必须）
 *  - 异步 CUDA stream 执行
 */
class TensorRTEngine {
public:
    TensorRTEngine();
    ~TensorRTEngine();

    /**
     * 从 ONNX 构建 TensorRT 引擎并序列化到磁盘
     * @param onnx_path    ONNX 模型路径
     * @param engine_path  输出 .trt 文件路径
     * @param fp16         是否启用 FP16
     * @param max_batch    最大 batch size
     * @param workspace    最大工作空间（字节）
     */
    bool buildFromONNX(const std::string& onnx_path,
                       const std::string& engine_path,
                       bool fp16 = true,
                       int max_batch = 1,
                       size_t workspace = 1UL << 28);

    /**
     * 加载已序列化的 TensorRT 引擎
     * @param engine_path .trt 文件路径
     */
    bool loadEngine(const std::string& engine_path);

    /**
     * 执行推理
     * @param input   输入数据指针 (float*, 大小 = batch * C * H * W)
     * @param outputs 输出数据数组 (vector<float*>)
     * @param batch   当前 batch 大小
     * @return 是否成功
     */
    bool infer(const float* input, std::vector<float*>& outputs, int batch = 1);

    /** 获取输入张量维度 (C, H, W) */
    std::vector<int> getInputShape() const { return _input_dims; }

    /** 获取各输出张量维度 */
    const std::vector<std::vector<int>>& getOutputShapes() const { return _output_dims; }

    /** 引擎是否就绪 */
    bool isReady() const { return _engine != nullptr && _context != nullptr; }

private:
    /** 分配 GPU 输入/输出 buffer */
    bool allocateBuffers();

    /** 释放所有资源 */
    void cleanup();

    // TRT 核心对象 — 用 shared_ptr 自定义 deleter 自动管理生命周期
    std::shared_ptr<nvinfer1::IRuntime>       _runtime;
    std::shared_ptr<nvinfer1::ICudaEngine>    _engine;
    std::shared_ptr<nvinfer1::IExecutionContext> _context;

    cudaStream_t    _stream = nullptr;

    // 输入/输出 buffer
    std::vector<void*>  _gpu_buffers;      // GPU 内存指针
    std::vector<void*>  _cpu_buffers;      // CPU 内存指针（固定内存，加速拷贝）
    std::vector<size_t> _buffer_sizes;     // 每个 buffer 的大小

    std::vector<int>                _input_dims;
    std::vector<std::vector<int>>   _output_dims;

    // Logger
    class TRTLogger : public nvinfer1::ILogger {
        void log(Severity severity, const char* msg) noexcept override;
    };
    TRTLogger _logger;
};
```

### 5.8 `src/trt_engine.cpp`

```cpp
#include "trt_engine.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <cuda_runtime.h>

// ============================================================
// Logger 实现
// ============================================================
void TensorRTEngine::TRTLogger::log(Severity severity, const char* msg) noexcept {
    if (severity == Severity::kERROR || severity == Severity::kINTERNAL_ERROR) {
        std::cerr << "[TensorRT] ERROR: " << msg << std::endl;
    } else if (severity == Severity::kWARNING) {
        std::cerr << "[TensorRT] WARNING: " << msg << std::endl;
    } else if (severity == Severity::kINFO) {
        std::cout << "[TensorRT] " << msg << std::endl;
    }
    // VERBOSE 忽略
}

// ============================================================
// 构造 / 析构
// ============================================================
TensorRTEngine::TensorRTEngine() {
    cudaStreamCreate(&_stream);
}

TensorRTEngine::~TensorRTEngine() {
    cleanup();
}

// ============================================================
// 从 ONNX 构建引擎
// ============================================================
bool TensorRTEngine::buildFromONNX(const std::string& onnx_path,
                                    const std::string& engine_path,
                                    bool fp16,
                                    int max_batch,
                                    size_t workspace)
{
    std::cout << "[TensorRT] 正在从 ONNX 构建引擎..." << std::endl;
    std::cout << "  ONNX:    " << onnx_path << std::endl;
    std::cout << "  Output:  " << engine_path << std::endl;
    std::cout << "  FP16:    " << (fp16 ? "ON" : "OFF") << std::endl;

    // ---- 创建 Builder ----
    auto builder = std::shared_ptr<nvinfer1::IBuilder>(
        nvinfer1::createInferBuilder(_logger),
        [](nvinfer1::IBuilder* p) { if (p) p->destroy(); });

    if (!builder) {
        std::cerr << "[TensorRT] ERROR: 创建 Builder 失败" << std::endl;
        return false;
    }

    // 显式 batch 维度
    const auto explicitBatch =
        1U << static_cast<uint32_t>(nvinfer1::NetworkDefinitionCreationFlag::kEXPLICIT_BATCH);

    auto network = std::shared_ptr<nvinfer1::INetworkDefinition>(
        builder->createNetworkV2(explicitBatch),
        [](nvinfer1::INetworkDefinition* p) { if (p) p->destroy(); });

    auto parser = std::shared_ptr<nvonnxparser::IParser>(
        nvonnxparser::createParser(*network, _logger),
        [](nvonnxparser::IParser* p) { if (p) p->destroy(); });

    // ---- 解析 ONNX ----
    std::ifstream onnx_file(onnx_path, std::ios::binary);
    if (!onnx_file.is_open()) {
        std::cerr << "[TensorRT] ERROR: 无法打开 ONNX 文件: " << onnx_path << std::endl;
        return false;
    }

    // 读取文件内容
    onnx_file.seekg(0, std::ios::end);
    size_t file_size = onnx_file.tellg();
    onnx_file.seekg(0, std::ios::beg);
    std::vector<char> onnx_data(file_size);
    onnx_file.read(onnx_data.data(), file_size);
    onnx_file.close();

    if (!parser->parse(onnx_data.data(), file_size)) {
        std::cerr << "[TensorRT] ERROR: ONNX 解析失败" << std::endl;
        for (int i = 0; i < parser->getNbErrors(); ++i) {
            std::cerr << "  " << parser->getError(i)->desc() << std::endl;
        }
        return false;
    }
    std::cout << "[TensorRT] ONNX 解析成功" << std::endl;

    // ---- 配置 ----
    auto config = std::shared_ptr<nvinfer1::IBuilderConfig>(
        builder->createBuilderConfig(),
        [](nvinfer1::IBuilderConfig* p) { if (p) p->destroy(); });

    config->setMaxWorkspaceSize(workspace);

    // FP16 模式（Jetson Nano 的 Volta GPU 有 FP16 Tensor Core）
    if (fp16 && builder->platformHasFastFp16()) {
        config->setFlag(nvinfer1::BuilderFlag::kFP16);
        std::cout << "[TensorRT] FP16 推理已启用" << std::endl;
    } else if (fp16) {
        std::cout << "[TensorRT] WARNING: 平台不支持 FP16，回退到 FP32" << std::endl;
    }

    // ---- 构建引擎 ----
    std::cout << "[TensorRT] 正在构建引擎（可能需要几分钟）..." << std::endl;

    auto engine = std::shared_ptr<nvinfer1::ICudaEngine>(
        builder->buildEngineWithConfig(*network, *config),
        [](nvinfer1::ICudaEngine* p) { if (p) p->destroy(); });

    if (!engine) {
        std::cerr << "[TensorRT] ERROR: 引擎构建失败" << std::endl;
        return false;
    }

    // ---- 序列化保存 ----
    auto serialized = std::shared_ptr<nvinfer1::IHostMemory>(
        engine->serialize(),
        [](nvinfer1::IHostMemory* p) { if (p) p->destroy(); });

    std::ofstream out_file(engine_path, std::ios::binary);
    if (!out_file.is_open()) {
        std::cerr << "[TensorRT] ERROR: 无法写入引擎文件: " << engine_path << std::endl;
        return false;
    }
    out_file.write(static_cast<const char*>(serialized->data()), serialized->size());
    out_file.close();

    std::cout << "[TensorRT] 引擎已保存至 " << engine_path
              << " (" << serialized->size() / (1024 * 1024) << " MB)" << std::endl;

    // 设置为当前引擎
    _engine = engine;
    _runtime = nullptr; // build 阶段不需要 runtime
    _context.reset(
        _engine->createExecutionContext(),
        [](nvinfer1::IExecutionContext* p) { if (p) p->destroy(); });

    return allocateBuffers();
}

// ============================================================
// 加载已序列化引擎
// ============================================================
bool TensorRTEngine::loadEngine(const std::string& engine_path) {
    std::cout << "[TensorRT] 加载引擎: " << engine_path << std::endl;

    // 读取文件
    std::ifstream file(engine_path, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "[TensorRT] ERROR: 无法打开引擎文件: " << engine_path << std::endl;
        return false;
    }

    file.seekg(0, std::ios::end);
    size_t size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<char> engine_data(size);
    file.read(engine_data.data(), size);
    file.close();

    // 创建 Runtime
    _runtime.reset(
        nvinfer1::createInferRuntime(_logger),
        [](nvinfer1::IRuntime* p) { if (p) p->destroy(); });

    // 反序列化
    _engine.reset(
        _runtime->deserializeCudaEngine(engine_data.data(), size),
        [](nvinfer1::ICudaEngine* p) { if (p) p->destroy(); });

    if (!_engine) {
        std::cerr << "[TensorRT] ERROR: 引擎反序列化失败" << std::endl;
        return false;
    }

    _context.reset(
        _engine->createExecutionContext(),
        [](nvinfer1::IExecutionContext* p) { if (p) p->destroy(); });

    std::cout << "[TensorRT] 引擎加载完成" << std::endl;
    return allocateBuffers();
}

// ============================================================
// 分配输入/输出 buffer
// ============================================================
bool TensorRTEngine::allocateBuffers() {
    if (!_engine || !_context) {
        std::cerr << "[TensorRT] ERROR: 引擎未就绪，无法分配 buffer" << std::endl;
        return false;
    }

    int num_bindings = _engine->getNbBindings();
    _gpu_buffers.resize(num_bindings, nullptr);
    _cpu_buffers.resize(num_bindings, nullptr);
    _buffer_sizes.resize(num_bindings, 0);

    _input_dims.clear();
    _output_dims.clear();

    for (int i = 0; i < num_bindings; ++i) {
        auto dims = _engine->getBindingDimensions(i);
        auto name = _engine->getBindingName(i);
        size_t size = 1;

        // 处理动态维度: 将 -1 替换为 1
        std::vector<int> dim_vec;
        for (int d = 0; d < dims.nbDims; ++d) {
            int val = dims.d[d];
            if (val == -1) val = 1;  // 动态维度取 1
            dim_vec.push_back(val);
            size *= val;
        }

        // 乘以数据类型大小
        auto dtype = _engine->getBindingDataType(i);
        switch (dtype) {
            case nvinfer1::DataType::kFLOAT: size *= sizeof(float); break;
            case nvinfer1::DataType::kHALF:  size *= sizeof(uint16_t); break;
            case nvinfer1::DataType::kINT8:  size *= sizeof(int8_t); break;
            case nvinfer1::DataType::kINT32: size *= sizeof(int32_t); break;
            default: size *= sizeof(float); break;
        }

        // 分配 GPU 内存
        cudaMalloc(&_gpu_buffers[i], size);

        // 分配 CPU 固定内存（pinned memory，加速 Host↔Device 传输）
        cudaMallocHost(&_cpu_buffers[i], size);

        _buffer_sizes[i] = size;

        if (_engine->bindingIsInput(i)) {
            _input_dims = dim_vec;
            std::cout << "[TensorRT] 输入 [" << name << "]: "
                      << dim_vec[0] << "x" << dim_vec[1] << "x"
                      << dim_vec[2] << "x" << dim_vec[3]
                      << " (" << size / 1024.0f << " KB)" << std::endl;
        } else {
            _output_dims.push_back(dim_vec);
            std::cout << "[TensorRT] 输出 [" << name << "]: [";
            for (size_t d = 0; d < dim_vec.size(); ++d) {
                if (d > 0) std::cout << "x";
                std::cout << dim_vec[d];
            }
            std::cout << "] (" << size / 1024.0f << " KB)" << std::endl;
        }
    }

    std::cout << "[TensorRT] Buffer 分配完成, 总计 "
              << (std::accumulate(_buffer_sizes.begin(), _buffer_sizes.end(), 0ULL) / (1024.0f * 1024.0f))
              << " MB" << std::endl;
    return true;
}

// ============================================================
// 推理
// ============================================================
bool TensorRTEngine::infer(const float* input,
                            std::vector<float*>& outputs,
                            int batch)
{
    if (!_context) {
        std::cerr << "[TensorRT] ERROR: 引擎未就绪" << std::endl;
        return false;
    }

    int num_bindings = _engine->getNbBindings();

    // ---- 拷贝输入到 GPU ----
    // 找到输入 binding 的索引
    int input_idx = -1;
    for (int i = 0; i < num_bindings; ++i) {
        if (_engine->bindingIsInput(i)) {
            input_idx = i;
            break;
        }
    }

    if (input_idx < 0) {
        std::cerr << "[TensorRT] ERROR: 未找到输入 binding" << std::endl;
        return false;
    }

    // CPU → GPU (异步)
    cudaMemcpyAsync(_gpu_buffers[input_idx], input,
                    _buffer_sizes[input_idx],
                    cudaMemcpyHostToDevice, _stream);

    // ---- 设置动态输入维度（如有） ----
    auto input_dims = _engine->getBindingDimensions(input_idx);
    if (input_dims.d[0] == -1) {
        // batch 维度是动态的
        nvinfer1::Dims4 set_dims{batch,
                                  _input_dims.size() > 1 ? _input_dims[1] : 3,
                                  _input_dims.size() > 2 ? _input_dims[2] : 640,
                                  _input_dims.size() > 3 ? _input_dims[3] : 640};
        _context->setBindingDimensions(input_idx, set_dims);
    }

    // ---- 执行推理 ----
    bool success = _context->enqueueV2(
        reinterpret_cast<void**>(_gpu_buffers.data()), _stream, nullptr);

    if (!success) {
        std::cerr << "[TensorRT] ERROR: 推理执行失败 (enqueueV2)" << std::endl;
        return false;
    }

    // ---- 拷贝输出回 CPU ----
    outputs.clear();
    outputs.resize(num_bindings - 1); // 减去输入

    int out_idx = 0;
    for (int i = 0; i < num_bindings; ++i) {
        if (!_engine->bindingIsInput(i)) {
            cudaMemcpyAsync(_cpu_buffers[i], _gpu_buffers[i],
                            _buffer_sizes[i],
                            cudaMemcpyDeviceToHost, _stream);
            outputs[out_idx] = static_cast<float*>(_cpu_buffers[i]);
            ++out_idx;
        }
    }

    // 等待所有异步操作完成
    cudaStreamSynchronize(_stream);

    return true;
}

// ============================================================
// 清理
// ============================================================
void TensorRTEngine::cleanup() {
    // 释放 GPU buffer
    for (auto& buf : _gpu_buffers) {
        if (buf) cudaFree(buf);
    }
    _gpu_buffers.clear();

    // 释放 CPU pinned buffer
    for (auto& buf : _cpu_buffers) {
        if (buf) cudaFreeHost(buf);
    }
    _cpu_buffers.clear();

    _buffer_sizes.clear();

    // 释放 TRT 对象（shared_ptr 自动管理）
    _context.reset();
    _engine.reset();
    _runtime.reset();

    if (_stream) {
        cudaStreamDestroy(_stream);
        _stream = nullptr;
    }

    std::cout << "[TensorRT] 资源已释放" << std::endl;
}
```

### 5.9 `include/postprocessor.h`

```cpp
#pragma once

#include <opencv2/opencv.hpp>
#include <string>
#include <vector>
#include <array>
#include <functional>

/**
 * 后处理与瑕疵判定模块
 *
 * 负责:
 *  1. 解码模型输出 (boxes, scores, class_ids)
 *  2. NMS 去重
 *  3. 严重程度判定 (OK / WARN / NG)
 *  4. 可视化画框
 *  5. 保存瑕疵图片
 */

// 缺陷检测结果
struct DefectResult {
    int     class_id;
    float   confidence;
    cv::Rect box;                   // x, y, w, h
    std::string class_name;
    std::string severity;           // "ok" / "warn" / "ng"
    std::string reason;
};

enum class Severity {
    OK = 0,
    WARN = 1,
    NG = 2,
};

class Postprocessor {
public:
    Postprocessor(float conf_threshold = 0.5f,
                  float nms_threshold = 0.4f,
                  const std::vector<std::string>& class_names = {"background", "defect"});

    /**
     * 完整的后处理流水线
     * @param raw_outputs     模型原始输出
     * @param num_outputs     输出 tensor 数量
     * @param output_sizes    每个输出 tensor 的字节数
     * @param display_frame   显示用图像（被标注）
     * @param input_w         模型输入宽度
     * @param input_h         模型输入高度
     * @return 检测到的所有缺陷
     */
    std::vector<DefectResult> process(float** raw_outputs,
                                       int num_outputs,
                                       const std::vector<size_t>& output_sizes,
                                       cv::Mat& display_frame,
                                       int input_w, int input_h);

    /**
     * 单独调用: 解析 + NMS（不解码模型输出时可复用）
     */
    std::vector<DefectResult> decodeAndNMS(float** raw_outputs,
                                            int num_outputs,
                                            const std::vector<size_t>& output_sizes,
                                            int frame_h, int frame_w,
                                            int input_h, int input_w);

    /**
     * 判定瑕疵严重程度（规则引擎）
     */
    Severity checkSeverity(const DefectResult& defect,
                           const cv::Size& frame_size);

    /**
     * 画检测框和标签
     */
    void drawResults(cv::Mat& frame,
                     const std::vector<DefectResult>& results);

    /**
     * 保存瑕疵图片
     * @param frame    标注后的图像
     * @param results  检测结果
     * @param out_dir  输出目录
     * @return 保存的文件路径，为空表示未保存
     */
    std::string saveDefectImage(const cv::Mat& frame,
                                const std::vector<DefectResult>& results,
                                const std::string& out_dir = "./output/");

private:
    float _conf_threshold;
    float _nms_threshold;
    std::vector<std::string> _class_names;

    // 用于 NMS 的辅助结构
    struct Detection {
        cv::Rect box;
        float    confidence;
        int      class_id;

        bool operator<(const Detection& other) const {
            return confidence > other.confidence; // 降序排列
        }
    };

    /** 计算两个矩形的 IoU */
    static float computeIoU(const cv::Rect& a, const cv::Rect& b);

    /** 自定义 NMS 实现 */
    std::vector<Detection> applyNMS(const std::vector<Detection>& detections);

    /** 获取当前时间戳字符串 */
    static std::string timestamp();
};
```

### 5.10 `src/postprocessor.cpp`

```cpp
#include "postprocessor.h"
#include "config.h"

#include <iostream>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <sys/stat.h>

// ============================================================
// 构造
// ============================================================
Postprocessor::Postprocessor(float conf_threshold,
                             float nms_threshold,
                             const std::vector<std::string>& class_names)
    : _conf_threshold(conf_threshold)
    , _nms_threshold(nms_threshold)
    , _class_names(class_names)
{
}

// ============================================================
// 完整后处理流水线
// ============================================================
std::vector<DefectResult> Postprocessor::process(
    float** raw_outputs,
    int num_outputs,
    const std::vector<size_t>& output_sizes,
    cv::Mat& display_frame,
    int input_w, int input_h)
{
    // Step 1: 解码 + NMS
    auto defects = decodeAndNMS(raw_outputs, num_outputs, output_sizes,
                                display_frame.rows, display_frame.cols,
                                input_h, input_w);

    // Step 2: 判定严重程度
    cv::Size frame_size(display_frame.cols, display_frame.rows);
    for (auto& d : defects) {
        Severity sev = checkSeverity(d, frame_size);
        switch (sev) {
            case Severity::OK:  d.severity = "ok";   break;
            case Severity::WARN: d.severity = "warn"; break;
            case Severity::NG:   d.severity = "ng";   break;
        }
    }

    // Step 3: 画框
    drawResults(display_frame, defects);

    return defects;
}

// ============================================================
// 模型输出解码 + NMS
//
// 假定输出格式为 YOLO 风格:
//   output[0]: float[N][5+num_classes]
//   每行: [cx, cy, w, h, obj_conf, class_0_conf, class_1_conf, ...]
// ============================================================
std::vector<DefectResult> Postprocessor::decodeAndNMS(
    float** raw_outputs,
    int num_outputs,
    const std::vector<size_t>& output_sizes,
    int frame_h, int frame_w,
    int input_h, int input_w)
{
    std::vector<Detection> detections;

    // 遍历所有输出层（多尺度输出模型）
    for (int o = 0; o < num_outputs; ++o) {
        float* output = raw_outputs[o];
        size_t size = output_sizes[o];

        // 推断该层的网格尺寸:
        // 输出总元素 = N * (5 + num_classes)
        // 每个检测点 = 5 + num_classes 个 float
        int num_classes = static_cast<int>(_class_names.size()) - 1; // 减 background
        int stride_size = 5 + num_classes;
        int num_detections = static_cast<int>(size / sizeof(float)) / stride_size;

        for (int i = 0; i < num_detections; ++i) {
            float* det = output + i * stride_size;

            float cx     = det[0];
            float cy     = det[1];
            float w      = det[2];
            float h      = det[3];
            float obj_conf = det[4];

            // 找最大类别分数
            float max_cls_score = 0.0f;
            int   max_cls_id    = 0;
            for (int c = 0; c < num_classes; ++c) {
                float score = det[5 + c];
                if (score > max_cls_score) {
                    max_cls_score = score;
                    max_cls_id = c + 1; // 跳过 background，class_id 从 1 开始
                }
            }

            float confidence = obj_conf * max_cls_score;
            if (confidence < _conf_threshold) continue;

            // 坐标从归一化转为像素值（相对于输入尺寸）
            float x1 = (cx - w / 2.0f) * input_w;
            float y1 = (cy - h / 2.0f) * input_h;
            float x2 = (cx + w / 2.0f) * input_w;
            float y2 = (cy + h / 2.0f) * input_h;

            // 裁剪到有效范围
            x1 = std::max(0.0f, std::min(x1, static_cast<float>(input_w)));
            y1 = std::max(0.0f, std::min(y1, static_cast<float>(input_h)));
            x2 = std::max(0.0f, std::min(x2, static_cast<float>(input_w)));
            y2 = std::max(0.0f, std::min(y2, static_cast<float>(input_h)));

            cv::Rect box;
            box.x = static_cast<int>(x1);
            box.y = static_cast<int>(y1);
            box.width  = static_cast<int>(x2 - x1);
            box.height = static_cast<int>(y2 - y1);

            if (box.width <= 0 || box.height <= 0) continue;

            Detection d;
            d.box = box;
            d.confidence = confidence;
            d.class_id = max_cls_id;
            detections.push_back(d);
        }
    }

    // ---- NMS ----
    auto nms_results = applyNMS(detections);

    // ---- 缩放到显示帧尺寸 ----
    float scale_x = static_cast<float>(frame_w) / input_w;
    float scale_y = static_cast<float>(frame_h) / input_h;

    std::vector<DefectResult> results;
    for (const auto& d : nms_results) {
        DefectResult r;
        r.class_id   = d.class_id;
        r.confidence = d.confidence;
        r.box = cv::Rect(
            static_cast<int>(d.box.x * scale_x),
            static_cast<int>(d.box.y * scale_y),
            static_cast<int>(d.box.width  * scale_x),
            static_cast<int>(d.box.height * scale_y)
        );
        r.class_name = (d.class_id < static_cast<int>(_class_names.size()))
                           ? _class_names[d.class_id]
                           : "unknown";
        results.push_back(r);
    }

    return results;
}

// ============================================================
// NMS
// ============================================================
std::vector<Postprocessor::Detection>
Postprocessor::applyNMS(const std::vector<Detection>& detections)
{
    if (detections.empty()) return {};

    // 按置信度降序排列
    auto sorted = detections;
    std::sort(sorted.begin(), sorted.end());

    std::vector<Detection> keep;
    std::vector<bool> suppressed(sorted.size(), false);

    for (size_t i = 0; i < sorted.size(); ++i) {
        if (suppressed[i]) continue;

        keep.push_back(sorted[i]);

        // 抑制与当前框 IoU 过高的后续框（同类才抑制）
        for (size_t j = i + 1; j < sorted.size(); ++j) {
            if (suppressed[j]) continue;
            // 不同类别不互相抑制
            if (sorted[i].class_id != sorted[j].class_id) continue;

            float iou = computeIoU(sorted[i].box, sorted[j].box);
            if (iou > _nms_threshold) {
                suppressed[j] = true;
            }
        }
    }

    return keep;
}

// ============================================================
// IoU 计算
// ============================================================
float Postprocessor::computeIoU(const cv::Rect& a, const cv::Rect& b) {
    int inter_x1 = std::max(a.x, b.x);
    int inter_y1 = std::max(a.y, b.y);
    int inter_x2 = std::min(a.x + a.width,  b.x + b.width);
    int inter_y2 = std::min(a.y + a.height, b.y + b.height);

    int inter_w = std::max(0, inter_x2 - inter_x1);
    int inter_h = std::max(0, inter_y2 - inter_y1);
    float inter_area = static_cast<float>(inter_w * inter_h);

    float area_a = static_cast<float>(a.width * a.height);
    float area_b = static_cast<float>(b.width * b.height);
    float union_area = area_a + area_b - inter_area;

    return (union_area > 0.0f) ? (inter_area / union_area) : 0.0f;
}

// ============================================================
// 规则引擎: 瑕疵严重程度判定
// ============================================================
Severity Postprocessor::checkSeverity(const DefectResult& defect,
                                       const cv::Size& frame_size)
{
    float area = static_cast<float>(defect.box.width * defect.box.height);
    float frame_area = static_cast<float>(frame_size.width * frame_size.height);
    float area_ratio = area / frame_area;

    const auto& cls = defect.class_name;

    // ---- 裂纹: 任意大小都 NG ----
    if (cls == "crack") {
        return Severity::NG;
    }

    // ---- 孔洞: 面积过大 → NG ----
    if (cls == "hole") {
        if (area > Config::Rule::HOLE_MAX_AREA) {
            return Severity::NG;
        }
        return Severity::WARN;
    }

    // ---- 节疤: 分级判定 ----
    if (cls == "knot") {
        if (area_ratio > Config::Rule::KNOT_NG_RATIO)   return Severity::NG;
        if (area_ratio > Config::Rule::KNOT_WARN_RATIO) return Severity::WARN;
        return Severity::OK;
    }

    // ---- 划痕: 长宽比 + 长度判定 ----
    if (cls == "scratch") {
        float max_len = static_cast<float>(std::max(defect.box.width,
                                                     defect.box.height));
        float min_len = static_cast<float>(std::min(defect.box.width,
                                                     defect.box.height));
        float aspect = max_len / (min_len + 1e-6f);

        if (aspect > Config::Rule::SCRATCH_ASPECT &&
            max_len > Config::Rule::SCRATCH_NG_LENGTH) {
            return Severity::NG;
        }
        if (max_len > Config::Rule::SCRATCH_WARN_LENGTH) {
            return Severity::WARN;
        }
        return Severity::OK;
    }

    // ---- 边角破损 → NG ----
    if (cls == "edge_damage") {
        return Severity::NG;
    }

    // ---- 腐朽 → NG ----
    if (cls == "rot") {
        return Severity::NG;
    }

    // ---- 污渍: 大面积 → NG ----
    if (cls == "stain") {
        if (area_ratio > Config::Rule::STAIN_NG_RATIO) {
            return Severity::NG;
        }
        return Severity::WARN;
    }

    return Severity::OK;
}

// ============================================================
// 可视化: 画框
// ============================================================
void Postprocessor::drawResults(cv::Mat& frame,
                                 const std::vector<DefectResult>& results)
{
    // 瑕疵颜色表
    static const std::map<std::string, cv::Scalar> COLORS = {
        {"knot",         cv::Scalar(0, 255, 255)},   // 黄
        {"crack",        cv::Scalar(0, 0, 255)},     // 红
        {"hole",         cv::Scalar(255, 0, 0)},     // 蓝
        {"stain",        cv::Scalar(255, 0, 255)},   // 紫
        {"scratch",      cv::Scalar(0, 165, 255)},   // 橙
        {"edge_damage",  cv::Scalar(0, 0, 128)},     // 深红
        {"rot",          cv::Scalar(128, 0, 128)},   // 深紫
    };

    for (const auto& defect : results) {
        if (defect.class_id == 0) continue; // skip background

        cv::Scalar color(0, 255, 0); // 默认绿色
        auto it = COLORS.find(defect.class_name);
        if (it != COLORS.end()) {
            color = it->second;
        }

        // 画矩形框
        cv::rectangle(frame,
                      cv::Point(defect.box.x, defect.box.y),
                      cv::Point(defect.box.x + defect.box.width,
                                defect.box.y + defect.box.height),
                      color, 2);

        // 标签文字
        std::ostringstream label;
        label << defect.class_name << ": "
              << std::fixed << std::setprecision(2) << defect.confidence
              << " [" << defect.severity << "]";

        int baseline = 0;
        cv::Size text_size = cv::getTextSize(label.str(),
                                              cv::FONT_HERSHEY_SIMPLEX,
                                              0.5, 1, &baseline);

        // 标签背景
        cv::rectangle(frame,
                      cv::Point(defect.box.x, defect.box.y - text_size.height - 8),
                      cv::Point(defect.box.x + text_size.width + 4, defect.box.y),
                      color, cv::FILLED);

        // 标签文字（黑底白字或白底黑字）
        cv::putText(frame, label.str(),
                    cv::Point(defect.box.x + 2, defect.box.y - 4),
                    cv::FONT_HERSHEY_SIMPLEX, 0.5,
                    cv::Scalar(255, 255, 255), 1);
    }
}

// ============================================================
// 保存瑕疵图片
// ============================================================
std::string Postprocessor::saveDefectImage(
    const cv::Mat& frame,
    const std::vector<DefectResult>& results,
    const std::string& out_dir)
{
    // 检查是否有 NG
    bool has_ng = false, has_warn = false;
    for (const auto& r : results) {
        if (r.severity == "ng")   { has_ng   = true; break; }
        if (r.severity == "warn") { has_warn = true; }
    }
    if (!has_ng && !has_warn) return "";

    // 创建输出目录
    if (mkdir(out_dir.c_str(), 0755) != 0 && errno != EEXIST) {
        std::cerr << "[Postprocessor] WARNING: 无法创建输出目录 " << out_dir << std::endl;
        return "";
    }

    // 生成文件名
    std::string prefix = has_ng ? "NG" : "WARN";
    std::string filename = out_dir + "/" + prefix + "_" + timestamp() + ".jpg";

    if (cv::imwrite(filename, frame)) {
        std::cout << "[Postprocessor] 瑕疵图片已保存: " << filename << std::endl;
        return filename;
    }

    std::cerr << "[Postprocessor] ERROR: 写入图片失败 " << filename << std::endl;
    return "";
}

std::string Postprocessor::timestamp() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()) % 1000;

    std::ostringstream oss;
    oss << std::put_time(std::localtime(&t), "%Y%m%d_%H%M%S")
        << "_" << std::setfill('0') << std::setw(3) << ms.count();
    return oss.str();
}
```

### 5.11 `src/main.cpp`

```cpp
/**
 * 木板瑕疵检测系统 - 主程序
 *
 * 运行于 NVIDIA Jetson Nano
 * 流水线实时检测木板表面缺陷: 节疤/裂纹/孔洞/污渍/划痕/边角破损/腐朽
 *
 * 数据流:
 *   海康工业相机 → MVS SDK 采集 → Bayer→BGR
 *   → CLAHE 增强 → TensorRT FP16 推理 → NMS 后处理
 *   → 瑕疵判定 → 显示/保存/PLC剔除
 *
 * 编译:
 *   mkdir build && cd build
 *   cmake -DCMAKE_BUILD_TYPE=Release ..
 *   make -j$(nproc)
 *
 * 运行:
 *   ./wood_defect_detector
 */

#include <opencv2/opencv.hpp>
#include <iostream>
#include <iomanip>
#include <cstring>
#include <csignal>
#include <atomic>
#include <chrono>
#include <thread>
#include <deque>
#include <numeric>
#include <fstream>

#include "config.h"
#include "camera.h"
#include "preprocessor.h"
#include "trt_engine.h"
#include "postprocessor.h"

// ============================================================
// 全局状态
// ============================================================
static std::atomic<bool> g_running{true};

void signalHandler(int sig) {
    std::cout << "\n[Main] 收到信号 " << sig << ", 正在安全退出..." << std::endl;
    g_running.store(false);
}

// ============================================================
// FPS 计数器
// ============================================================
class FPSCounter {
public:
    void tick(double elapsed_ms) {
        _history.push_back(elapsed_ms);
        if (_history.size() > 100) _history.pop_front();
    }

    double fps() const {
        if (_history.empty()) return 0.0;
        double avg_ms = std::accumulate(_history.begin(), _history.end(), 0.0)
                        / _history.size();
        return (avg_ms > 0.0) ? 1000.0 / avg_ms : 0.0;
    }

    double avgLatencyMs() const {
        if (_history.empty()) return 0.0;
        return std::accumulate(_history.begin(), _history.end(), 0.0)
               / _history.size();
    }

private:
    std::deque<double> _history;
};

// ============================================================
// 首次运行: 检测 .trt 引擎是否存在，不存在则从 ONNX 构建
// ============================================================
bool ensureEngine(TensorRTEngine& engine) {
    std::ifstream trt_file(Config::MODEL_PATH, std::ios::binary);
    if (trt_file.good()) {
        trt_file.close();
        return engine.loadEngine(Config::MODEL_PATH);
    }

    // 引擎不存在，从 ONNX 构建
    std::cout << "\n[Main] ===================================" << std::endl;
    std::cout << "[Main] 未找到 TensorRT 引擎文件" << std::endl;
    std::cout << "[Main] 正在从 ONNX 构建（首次运行, 约需2-5分钟）..." << std::endl;
    std::cout << "[Main] ===================================\n" << std::endl;

    bool ok = engine.buildFromONNX(
        Config::ONNX_PATH,
        Config::MODEL_PATH,
        Config::USE_FP16,
        1,
        static_cast<size_t>(Config::MAX_WORKSPACE_SIZE)
    );

    if (ok) {
        std::cout << "\n[Main] TensorRT 引擎构建完成！" << std::endl;
    }
    return ok;
}

// ============================================================
// 打印启动横幅
// ============================================================
void printBanner() {
    std::cout << "\n========================================" << std::endl;
    std::cout << "  木 板 瑕 疵 检 测 系 统  v1.0" << std::endl;
    std::cout << "  NVIDIA Jetson Nano | C++ | TensorRT" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "  类别: ";
    for (size_t i = 1; i < Config::CLASS_NAMES.size(); ++i) {
        if (i > 1) std::cout << ", ";
        std::cout << Config::CLASS_NAMES[i];
    }
    std::cout << std::endl;
    std::cout << "========================================\n" << std::endl;
}

// ============================================================
// 主入口
// ============================================================
int main(int argc, char* argv[]) {
    // ---- 注册信号处理 ----
    std::signal(SIGINT,  signalHandler);
    std::signal(SIGTERM, signalHandler);

    printBanner();

    // ========================================================
    // 1. 初始化海康工业相机
    // ========================================================
    HikvisionCamera camera;

    // 按序列号 / IP / 自定义名称连接（优先级: 序列号 > IP > 自定义名称）
    bool camera_ok = false;
    if (strlen(Config::CAMERA_SERIAL) > 0) {
        camera_ok = camera.connectBySerial(Config::CAMERA_SERIAL);
    } else if (strlen(Config::CAMERA_IP) > 0) {
        camera_ok = camera.connectByIP(Config::CAMERA_IP);
    } else if (strlen(Config::CAMERA_USER_ID) > 0) {
        camera_ok = camera.connectByUserID(Config::CAMERA_USER_ID);
    } else {
        // 不指定则连接第一个在线设备
        camera_ok = camera.connectByIP("");  // 空 IP 自动匹配首个设备
    }

    if (!camera_ok) {
        std::cerr << "[Main] FATAL: 海康相机连接失败" << std::endl;
        return -1;
    }

    // 启动采集（设置触发模式、曝光、增益）
    if (!camera.start(Config::CAMERA_WIDTH, Config::CAMERA_HEIGHT,
                       Config::CAMERA_EXPOSURE_US,
                       Config::CAMERA_GAIN_DB,
                       Config::CAMERA_TRIGGER_MODE)) {
        std::cerr << "[Main] FATAL: 相机取流启动失败" << std::endl;
        return -1;
    }

    // ========================================================
    // 2. 初始化 TensorRT 引擎
    // ========================================================
    TensorRTEngine engine;
    if (!ensureEngine(engine)) {
        std::cerr << "[Main] FATAL: TensorRT 引擎初始化失败" << std::endl;
        camera.stop();
        return -2;
    }

    // ========================================================
    // 3. 初始化预处理器
    // ========================================================
    Preprocessor preprocessor(Config::INPUT_WIDTH,
                              Config::INPUT_HEIGHT,
                              Config::ENABLE_CLAHE,
                              Config::CLAHE_CLIP_LIMIT,
                              Config::CLAHE_TILE_SIZE);

    // 预分配输入 tensor 内存（复用避免反复分配）
    size_t tensor_size = Config::INPUT_WIDTH * Config::INPUT_HEIGHT * Config::INPUT_CHANNELS;
    std::vector<float> input_tensor(tensor_size, 0.0f);

    // ========================================================
    // 4. 初始化后处理器
    // ========================================================
    Postprocessor postprocessor(Config::CONF_THRESHOLD,
                                Config::NMS_THRESHOLD,
                                Config::CLASS_NAMES);

    // ========================================================
    // 5. 主检测循环
    // ========================================================
    std::cout << "[Main] 系统就绪，开始检测...\n" << std::endl;

    FPSCounter fps_counter;
    uint64_t   total_frames   = 0;
    uint64_t   defect_frames  = 0;
    uint64_t   ng_count       = 0;
    auto       start_time     = std::chrono::steady_clock::now();

    std::vector<float*> outputs;       // 模型输出指针（复用）
    std::vector<size_t> output_sizes;  // 各输出大小

    // 获取输出维度信息
    const auto& out_shapes = engine.getOutputShapes();
    for (const auto& shape : out_shapes) {
        size_t s = sizeof(float);
        for (int d : shape) s *= d;
        output_sizes.push_back(s);
    }

    while (g_running.load()) {
        auto frame_start = std::chrono::steady_clock::now();

        // ---- Step 1: 采集帧 ----
        cv::Mat frame = camera.read();
        if (frame.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }

        // ---- Step 2: 预处理 ----
        cv::Mat display_frame;
        preprocessor.process(frame, input_tensor.data(), display_frame);

        // ---- Step 3: TensorRT 推理 ----
        if (!engine.infer(input_tensor.data(), outputs, 1)) {
            std::cerr << "[Main] 推理失败，跳过此帧" << std::endl;
            continue;
        }

        // ---- Step 4: 后处理 + 判定 ----
        auto defects = postprocessor.process(
            outputs.data(),
            static_cast<int>(outputs.size()),
            output_sizes,
            display_frame,
            Config::INPUT_WIDTH,
            Config::INPUT_HEIGHT);

        // ---- Step 5: 决策输出 ----
        bool has_ng   = false;
        bool has_warn = false;

        for (const auto& d : defects) {
            if (d.severity == "ng")   { has_ng   = true; break; }
            if (d.severity == "warn") { has_warn = true; }
        }

        if (has_ng) {
            ng_count++;
            // TODO: 通过 GPIO/串口 发送剔除信号给 PLC
            // gpioWrite(REJECT_PIN, HIGH);
            // delay_us(1000);
            // gpioWrite(REJECT_PIN, LOW);
        }

        if (!defects.empty()) {
            defect_frames++;
            if (Config::SAVE_DEFECT_IMAGES && has_ng) {
                postprocessor.saveDefectImage(display_frame, defects,
                                              Config::OUTPUT_DIR);
            }

            // 打印缺陷信息
            std::cout << "[DETECT] ";
            for (size_t i = 0; i < defects.size(); ++i) {
                if (i > 0) std::cout << " | ";
                std::cout << defects[i].class_name
                          << "(" << std::fixed << std::setprecision(2)
                          << defects[i].confidence << ")"
                          << " [" << defects[i].severity << "]";
            }
            std::cout << std::endl;
        }

        // ---- Step 6: 显示 ----
        if (Config::SHOW_DISPLAY) {
            // 叠加状态信息
            std::ostringstream status_oss;
            status_oss << "Status: " << (has_ng ? "NG" : has_warn ? "WARN" : "OK");
            cv::Scalar status_color = has_ng ? cv::Scalar(0, 0, 255)
                                     : has_warn ? cv::Scalar(0, 255, 255)
                                     : cv::Scalar(0, 255, 0);

            cv::putText(display_frame, status_oss.str(),
                        cv::Point(10, 30),
                        cv::FONT_HERSHEY_SIMPLEX, 1.0,
                        status_color, 2);

            std::ostringstream fps_oss;
            fps_oss << "FPS: " << std::fixed << std::setprecision(1)
                    << fps_counter.fps();
            cv::putText(display_frame, fps_oss.str(),
                        cv::Point(10, 60),
                        cv::FONT_HERSHEY_SIMPLEX, 0.6,
                        cv::Scalar(255, 255, 255), 1);

            cv::imshow("Wood Defect Detection", display_frame);

            int key = cv::waitKey(1) & 0xFF;
            if (key == 'q' || key == 27) {  // q 或 ESC
                g_running.store(false);
                std::cout << "[Main] 用户退出" << std::endl;
                break;
            }
        }

        // ---- FPS 统计 ----
        auto frame_end = std::chrono::steady_clock::now();
        double elapsed_ms = std::chrono::duration<double, std::milli>(
                                frame_end - frame_start).count();
        fps_counter.tick(elapsed_ms);
        total_frames++;

        if (Config::PRINT_FPS && total_frames % Config::FPS_LOG_INTERVAL == 0) {
            std::cout << "[Main] FPS: " << std::fixed << std::setprecision(1)
                      << fps_counter.fps()
                      << " | 平均延迟: " << std::setprecision(1)
                      << fps_counter.avgLatencyMs() << "ms"
                      << " | 已处理: " << total_frames << "帧"
                      << " | NG: " << ng_count
                      << std::endl;
        }
    }

    // ========================================================
    // 6. 清理 & 统计
    // ========================================================
    auto end_time = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(end_time - start_time).count();

    camera.stop();
    cv::destroyAllWindows();

    std::cout << "\n========================================" << std::endl;
    std::cout << "  系统已停止" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "  运行时长:    " << std::fixed << std::setprecision(1)
              << elapsed << "s" << std::endl;
    std::cout << "  处理帧数:    " << total_frames << std::endl;
    std::cout << "  平均 FPS:    " << std::setprecision(1)
              << (total_frames / std::max(elapsed, 0.001)) << std::endl;
    std::cout << "  瑕疵帧数:    " << defect_frames << std::endl;
    std::cout << "  NG 次数:     " << ng_count << std::endl;
    std::cout << "========================================" << std::endl;

    return 0;
}
```

### 5.12 `build.sh`

```bash
#!/bin/bash
# ============================================================
# 木板瑕疵检测系统 - 一键编译脚本
# 适用于 Jetson Nano (JetPack 4.x+)
# ============================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"

echo "========================================"
echo "  木板瑕疵检测系统 - 编译"
echo "========================================"

# 检查 TensorRT
if ! ldconfig -p | grep -q libnvinfer; then
    echo "ERROR: 未找到 TensorRT 库，请确认 JetPack 已正确安装"
    exit 1
fi

# 检查 OpenCV
if ! pkg-config --exists opencv4 2>/dev/null; then
    if ! pkg-config --exists opencv 2>/dev/null; then
        echo "ERROR: 未找到 OpenCV，请安装: sudo apt install libopencv-dev"
        exit 1
    fi
fi

# 创建 build 目录
mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

# CMake 配置
echo ""
echo "[1/2] CMake 配置..."
cmake -DCMAKE_BUILD_TYPE=Release "${SCRIPT_DIR}"

# 编译
echo ""
echo "[2/2] 编译中..."
make -j$(nproc)

echo ""
echo "========================================"
echo "  编译完成！"
echo "  可执行文件: ${BUILD_DIR}/wood_defect_detector"
echo ""
echo "  运行: ./build/wood_defect_detector"
echo "========================================"
```

---

## 6. 适配指南

### 6.1 换其他品牌的相机

只需要改 `camera.h` 和 `camera.cpp`。接口保持一致：

```cpp
class XxxCamera {
public:
    bool connectByXXX(...);   // 连接
    bool start(...);          // 开始取流
    void stop();              // 停止
    cv::Mat read();           // 非阻塞取帧（返回 BGR cv::Mat）
    bool isRunning() const;   // 运行状态
};
```

只要 `read()` 返回 `cv::Mat (BGR)`，其余模块（预处理/推理/后处理）完全不动。

### 6.2 换其他模型（非 YOLO 输出格式）

只需改 `postprocessor.cpp` 里的 `decodeAndNMS()` 函数，调整输出解析逻辑。其余模块不动。

### 6.3 增加新的瑕疵类别

只需改两处：

1. `config.h` 的 `CLASS_NAMES` 数组追加类别名
2. `postprocessor.cpp` 的 `checkSeverity()` 追加判定规则

### 6.4 调整判定阈值

全部在 `config.h` 的 `namespace Rule` 里：

```cpp
HOLE_MAX_AREA     = 100.0f;   // 调大 → 更宽容
KNOT_NG_RATIO     = 0.05f;   // 调小 → 更严格
SCRATCH_NG_LENGTH = 50.0f;   // 调小 → 更严格
```

### 6.5 连接PLC剔除

在 `main.cpp` 的检测循环里，`has_ng == true` 时发送信号。根据你的 PLC 通信方式：

```cpp
// GPIO 方式:
if (has_ng) {
    gpioWrite(REJECT_PIN, HIGH);
    usleep(50000);  // 50ms脉冲
    gpioWrite(REJECT_PIN, LOW);
}

// 串口/Modbus 方式:
if (has_ng) {
    serial.send("REJECT\n");  // 或 modbus.writeRegister(...)
}
```

### 6.6 性能调优

| 问题 | 方向 |
|------|------|
| FPS 不够 | 降低相机分辨率 → 减小 `INPUT_WIDTH/HEIGHT` → 开启 FP16 |
| 漏检太多 | 降低 `CONF_THRESHOLD` → 增加训练数据 |
| 误检太多 | 提高 `CONF_THRESHOLD` → 提高 `NMS_THRESHOLD` → 收紧规则阈值 |
| CLAHE 效果不好 | 调 `CLAHE_CLIP_LIMIT`（增大=更强对比度）或 `CLAHE_TILE_SIZE` |
| 图像模糊 | 减小曝光时间 `CAMERA_EXPOSURE_US`（线速度越快曝光越短） |
