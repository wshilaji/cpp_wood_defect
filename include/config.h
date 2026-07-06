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
