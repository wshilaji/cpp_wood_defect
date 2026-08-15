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

// ---- PLC Modbus TCP 通信（Nano=从站, PLC=主站） ----
// Holding Register 映射:
//   HR0 — TRIGGER : PLC 写 1 触发拍照，Nano 清 0
//   HR1 — RESULT   : 0=空闲, 1=OK, 2=NG
//   HR2 — STATUS   : 0=未就绪, 1=就绪
constexpr int         PLC_TCP_PORT     = 502;   // Modbus TCP 标准端口

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
constexpr float SCRATCH_NG_LEN  = 50.0f;    // shuwen/piwenba/baowen 纹类缺陷:长边超阈值且长宽比超阈值判 NG
constexpr float SCRATCH_ASPECT  = 5.0f;     // 纹类缺陷长宽比阈值

// ---- 聚合判定（数量 / 面积占比）----
constexpr int   JIEBA_MAX_COUNT  = 8;        // jieba 节疤:数量 > 此值判 NG
constexpr int   DONGBA_MAX_COUNT = 8;        // dongba 洞疤:数量 > 此值判 NG
constexpr float DONGBAN_AREA_RATIO = 0.01f;  // dongban 洞板:面积和占整图比例 > 1% 判 NG
constexpr float QUEBIAN_AREA_RATIO = 0.01f;  // quebian 缺边:面积和占整图比例 > 1% 判 NG
constexpr int   JIEBA_DONGBA_MAX_COUNT = 12;      // jieba+dongba 数量之和 > 此值判 NG
constexpr float DONGBAN_QUEBIAN_AREA_RATIO = 0.015f; // dongban+quebian 面积之和占比 > 1.5% 判 NG

// ---- 木板尺寸判定（测量长/宽低于阈值判 NG，默认整板长 1200 / 宽 600 的一半） ----
constexpr int   MIN_LENGTH_MM = 600;   // 板长 < 此值判 NG
constexpr int   MIN_WIDTH_MM  = 300;   // 板宽 < 此值判 NG

// ---- 相机标定（木板长宽测量） ----
// MV-CS050-60GC 像元尺寸 3.45μm (正方形)
// fx = fy = 8mm / 3.45μm ≈ 2319
constexpr float FX          = 2319.0f;
constexpr float FY          = 2319.0f;
constexpr float CX          = 1224.0f;   // 2448/2
constexpr float CY          = 1024.0f;   // 2048/2
constexpr float DISTANCE_MM = 1270.0f;   // 相机到木板距离 mm
const std::vector<double> DIST_COEFFS = {0.0, 0.0, 0.0, 0.0, 0.0};

// ---- 输出 ----
constexpr bool  SAVE_IMAGES   = true;
constexpr bool  SHOW_DISPLAY  = true;
constexpr const char* OUTPUT_DIR = "./output/";
constexpr const char* SAVE_ENABLE_PASSWORD = "629785";   // 界面开启「存图开关」所需密码

} // namespace Config
