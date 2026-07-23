#pragma once
#include <string>
#include <vector>

namespace Config {

// ---- 海康相机 ----
constexpr const char* CAMERA_IP        = "192.168.1.10";
constexpr int         CAMERA_WIDTH     = 1920;
constexpr int         CAMERA_HEIGHT    = 1080;
constexpr float       CAMERA_EXPOSURE  = 5000.0f;
constexpr float       CAMERA_GAIN      = 0.0f;
constexpr int         CAMERA_TRIGGER   = 1;  // 0=连续 1=软触发 2=硬触发

// ---- PLC TCP 通信 ----
constexpr int         PLC_TCP_PORT     = 5000;

// ---- 模型 ----
constexpr const char* ENGINE_PATH    = "models/defect_model.trt";
constexpr float       CONF_THRESHOLD = 0.5f;

// ---- 类别（与模型输出 class_id 对应，0起始） ----
const std::vector<std::string> CLASSES = {
    "knot", "crack", "hole", "stain", "scratch", "edge_damage", "rot"
};

// ---- 判定阈值 ----
constexpr float HOLE_MAX_AREA   = 100.0f;
constexpr float KNOT_NG_RATIO   = 0.05f;
constexpr float KNOT_WARN_RATIO = 0.01f;
constexpr float SCRATCH_NG_LEN  = 50.0f;
constexpr float SCRATCH_WARN_LEN= 30.0f;
constexpr float SCRATCH_ASPECT  = 5.0f;
constexpr float STAIN_NG_RATIO  = 0.03f;

// ---- 输出 ----
constexpr bool  SAVE_IMAGES   = true;
constexpr bool  SHOW_DISPLAY  = true;
constexpr const char* OUTPUT_DIR = "./output/";

} // namespace Config
