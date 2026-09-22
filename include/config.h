#pragma once
#include <string>
#include <vector>

namespace Config {

// ---- 海康相机 ----
constexpr const char* CAMERA_IP        = "192.168.2.10";
constexpr int         CAMERA_WIDTH     = 2448;   // MV-CS050-60GC 原生分辨率
constexpr int         CAMERA_HEIGHT    = 2048;
constexpr float       CAMERA_EXPOSURE  = 6000.0f;
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
// 现场叫法(界面/工人口头用的)与下面拼音类名的对应, 代码里只有拼音, 记这里免得回头认不出:
//   jieba   活节   —— 木节发白、按不掉, 不影响使用
//   dongba  死节   —— 节扣没掉, 但使劲一按就掉
//   heiba   小油疤 —— 黑色油滴在板面, 板子不碎; 单个没事, 数量多了才扔
//   dongban 漏洞   —— 节扣掉了板子被穿透, 底下黑传送带透出来; **大油疤也标成这一类**
//   quebian 缺边
// 其余(shupi/shuwen/piwenba/baowen/liefeng/suibian/heiban/banwen/banwenba)
// 目前只在图上画框显示, 不参与 NG 判定 —— 判定逻辑见 postprocessor.cpp 的 isNG()。
// 注意 34-35 行的 SCRATCH_NG_LEN / SCRATCH_ASPECT 是给纹类(shuwen/piwenba/baowen)准备的,
// 常量定义了但 isNG() 里从来没实现, 现在全代码无引用。
//
// ⚠ 顺序不能动: 这里的下标就是模型的 class_id, 改顺序等于把模型输出对错类。
const std::vector<std::string> CLASSES = {
    "dongba", "dongban", "jieba", "shupi", "shuwen",
    "heiba", "piwenba", "quebian", "baowen", "liefeng",
    "suibian", "heiban", "banwen", "banwenba"
};

// ---- 判定阈值 ----
constexpr float SCRATCH_NG_LEN  = 50.0f;    // shuwen/piwenba/baowen 纹类缺陷:长边超阈值且长宽比超阈值判 NG
constexpr float SCRATCH_ASPECT  = 5.0f;     // 纹类缺陷长宽比阈值

// ---- 聚合判定（数量 / 面积占比）----
// 这些是「没有 config.ini 时」的出厂默认值，唯一权威来源是界面上的工人设置
// （每块板都会把界面值下发给后处理），改这里只是让默认值和现场调好的那套对齐。
// 现场 config.ini: jieba_max=10 dongba_max=2 heiba_max=24
//                 dongban_area_pct=0.2 quebian_area_pct=0.5
//                 jieba_dongba_max=6 dongban_quebian_area_pct=0.4
constexpr int   JIEBA_MAX_COUNT  = 10;       // jieba 节疤:数量 > 此值判 NG
constexpr int   DONGBA_MAX_COUNT = 2;        // dongba 死节:数量 > 此值判 NG
constexpr int   HEIBA_MAX_COUNT  = 24;       // heiba 小油疤:数量 > 此值判 NG
constexpr float DONGBAN_AREA_RATIO = 0.002f; // dongban 洞板:面积和占整图比例 > 0.2% 判 NG
constexpr float QUEBIAN_AREA_RATIO = 0.005f; // quebian 缺边:面积和占整图比例 > 0.5% 判 NG
constexpr int   JIEBA_DONGBA_MAX_COUNT = 6;       // jieba+dongba 数量之和 > 此值判 NG
constexpr float DONGBAN_QUEBIAN_AREA_RATIO = 0.004f; // dongban+quebian 面积之和占比 > 0.4% 判 NG

// ---- 木板尺寸判定（测量长/宽低于阈值判 NG）----
// 现场 min_len_mm=1200 / min_wid_mm=600 = 整板尺寸，也就是「比整板小就判 NG」，
// 不再是原先的「整板一半」。注意这会让判定贴着整板尺寸走，测量本身有误差时
// 边缘板会来回翻，真要卡这么紧得先确认测量精度。
constexpr int   MIN_LENGTH_MM = 1200;   // 板长 < 此值判 NG
constexpr int   MIN_WIDTH_MM  = 600;    // 板宽 < 此值判 NG

// ---- 相机标定（木板长宽测量） ----
// MV-CS050-60GC 像元尺寸 3.45μm (正方形)
// fx = fy = 8mm / 3.45μm ≈ 2319
constexpr float FX          = 2319.0f;
constexpr float FY          = 2319.0f;
constexpr float CX          = 1224.0f;   // 2448/2
constexpr float CY          = 1024.0f;   // 2048/2
constexpr float DISTANCE_MM = 1270.0f;   // 相机到木板距离 mm
const std::vector<double> DIST_COEFFS = {0.0, 0.0, 0.0, 0.0, 0.0};

// ---- 界面「退出」后的冷却时间 ----
// 点界面「退出」是为了腾出桌面(开 RustDesk / 远程协助 / 改网络)。程序收到退出请求
// 后先 hide() 掉全屏置顶窗口让桌面立刻可用, 但**进程不立刻结束**, 而是原地等这么久
// 再退出。systemd 是 Type=simple+Restart=always, 看进程还活着就不会重启, 所以
// "点退出 → 界面重新拉起"的总延时≈本值。崩溃路径不走这段等待(RestartSec 3s 恢复)。
constexpr int   EXIT_RESTART_DELAY_SEC = 150;   // 2.5 分钟

// ---- 输出 ----
constexpr bool  SAVE_IMAGES   = true;
constexpr bool  SHOW_DISPLAY  = true;
constexpr const char* OUTPUT_DIR = "./output/";
constexpr const char* SAVE_ENABLE_PASSWORD = "629785";   // 界面开启「存图开关」所需密码

} // namespace Config
