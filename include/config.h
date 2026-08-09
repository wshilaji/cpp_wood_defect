#pragma once
#include <string>
#include <vector>

namespace Config {

// ---- 海康相机 ----
constexpr const char* CAMERA_IP        = "192.168.2.10";
constexpr int         CAMERA_WIDTH     = 2448;   // MV-CS050-60GC 原生分辨率
constexpr int         CAMERA_HEIGHT    = 2048;
constexpr float       CAMERA_EXPOSURE  = 7000.0f;
constexpr float       CAMERA_GAIN      = 0.0f;
constexpr int         CAMERA_TRIGGER   = 1;  // 0=连续 1=软触发 2=硬触发

// ---- PLC TCP 通信 ----
constexpr int         PLC_TCP_PORT     = 5000;

// ---- 模型 ----
constexpr const char* ENGINE_PATH    = "models/best.engine";
constexpr float       CONF_THRESHOLD = 0.5f;

// ---- 类别（与模型输出 class_id 对应，0起始） ----
const std::vector<std::string> CLASSES = {
    "dongba", "dongban", "jieba", "shupi", "shuwen",
    "heiba", "piwenba", "quebian", "baowen", "liefeng",
    "suibian", "heiban", "banwen", "banwenba"
};

// ---- 判定阈值 ----
constexpr float HOLE_MAX_AREA   = 100.0f;   // dongba/dongban 洞类缺陷
constexpr float KNOT_NG_RATIO   = 0.05f;    // jieba 节疤
constexpr float KNOT_WARN_RATIO = 0.01f;
constexpr float SCRATCH_NG_LEN  = 50.0f;    // shuwen/baowen 纹类缺陷
constexpr float SCRATCH_WARN_LEN= 30.0f;
constexpr float SCRATCH_ASPECT  = 5.0f;
constexpr float STAIN_NG_RATIO  = 0.03f;    // heiba/heiban/banwen 黑/斑类缺陷

// ---- 相机标定（木板长宽测量，标定后替换实际值） ----
constexpr float FX          = 2000.0f;
constexpr float FY          = 2000.0f;
constexpr float CX          = 1224.0f;   // 2448/2
constexpr float CY          = 1024.0f;   // 2048/2
constexpr float DISTANCE_MM = 500.0f;    // 相机到木板距离 mm
const std::vector<double> DIST_COEFFS = {0.0, 0.0, 0.0, 0.0, 0.0};

// ---- 输出 ----
constexpr bool  SAVE_IMAGES   = true;
constexpr bool  SHOW_DISPLAY  = true;
constexpr const char* OUTPUT_DIR = "./output/";

} // namespace Config
