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
//   dongban 破洞   —— 节扣掉了板子被穿透, 底下黑传送带透出来; **大油疤也标成这一类**
//   quebian 缺边
//   shupi   树皮   —— 板面带树皮; 2026-09-23 起按面积和占比判 NG
//   fabai   发白   —— 2026-09-23 新加的类, 按面积和占比判 NG; 待现场补一句这个类长什么样
// 其余(shuwen/piwenba/baowen/liefeng/suibian/heiban/banwen/banwenba)
// 目前只在图上画框显示, 不参与 NG 判定 —— 判定逻辑见 postprocessor.cpp 的 isNG()。
// 注意 34-35 行的 SCRATCH_NG_LEN / SCRATCH_ASPECT 是给纹类(shuwen/piwenba/baowen)准备的,
// 常量定义了但 isNG() 里从来没实现, 现在全代码无引用。
//
// ⚠ 顺序不能动: 这里的下标就是模型的 class_id, 改顺序等于把模型输出对错类。
//   fabai 是【追加在最后】的(下标 14): 前面 0-13 一个没动, 所以现有那份 14 类的
//   best.engine / labels.txt 照跑不误 —— trtyolo 只吐类别 id, id→名字全在这个表里,
//   多出来的第 15 个名字没人会用上, 不会崩。
//   ⚠ 重训模型时, 新 labels.txt 必须让 fabai 也在最后一行(第 15 行)。顺序对不上不会
//   报任何错, 只会静默把类别判错 —— 这是本项目最容易出、最难查的一类事故。
const std::vector<std::string> CLASSES = {
    "dongba", "dongban", "jieba", "shupi", "shuwen",
    "heiba", "piwenba", "quebian", "baowen", "liefeng",
    "suibian", "heiban", "banwen", "banwenba",
    "fabai"
};

// ---- 判定阈值 ----
constexpr float SCRATCH_NG_LEN  = 50.0f;    // shuwen/piwenba/baowen 纹类缺陷:长边超阈值且长宽比超阈值判 NG
constexpr float SCRATCH_ASPECT  = 5.0f;     // 纹类缺陷长宽比阈值

// ---- 聚合判定（数量 + 尺寸门槛）----
// 这些是「没有 config.ini 时」的出厂默认值，唯一权威来源是界面上的工人设置
// （每块板都会把界面值下发给后处理），改这里只是让默认值和现场调好的那套对齐。
//
// ⚠ 2026-09-23 起本项目【没有面积规则了】。dongban/quebian/shupi/fabai 四个类原本都按
//   「面积和占整图比例」判, 现场在一天里逐个改成跟 dongba 死节同一个形状:
//        算数的块数 > MAX_COUNT 判 NG, 而「算不算数」由 MIN_LEN_MM 那道尺寸门槛决定
//        (检测框最长边换算成毫米, 短于门槛的不计数 —— 框照画, 只是颜色暗一档)。
//   两半是同一条规则的上下游: 先按尺寸筛, 再数个数, 只留一半没有意义。
//   为什么改: 面积和把「一堆小的」和「一个大的」算成同一个数 —— 30 个小油疤的面积和
//   可以大于 1 个大油疤, 但这两块板该不该扔正好相反。尺寸门槛 + 数量才分得开。
//
// 现场 config.ini: jieba_max=10 dongba_max=2 heiba_max=24 quebian_area_pct=0.5
//                 jieba_dongba_max=6
//                 (dongba_min_len_mm / dongban_* / shupi_* / fabai_* / quebian_max_count /
//                  quebian_min_len_mm 是 2026-09-23 新加的键, 现场还没填过, 没有 ini 时
//                  走下面的出厂默认值。dongban_area_pct / quebian_area_pct /
//                  dongban_quebian_area_pct 是同一天作废的键, 代码里已经不读 ——
//                  ini 里还留着不影响任何东西, 不用去清)

// 出厂值分两档, 区别是「这个类原来在不在判」:
//   原来就在判的(jieba/dongba/heiba/dongban/quebian) —— 照抄现场调过的数, 或现场那条
//     面积规则的等价值。注意面积改成数量【不是】等价换算(见各自的注释), 会变严会变松。
//   原来不在判的(shupi/fabai) —— 数量给 99 = 实际不判。数量规则里 0 = 一个都不许有,
//     而这两个类现场一个数都没调过, 给个真数就是上线即误杀。
constexpr int   JIEBA_MAX_COUNT  = 10;       // jieba 活节:数量 > 此值判 NG(没有尺寸门槛)
constexpr int   DONGBA_MAX_COUNT = 2;        // dongba 死节:算数的块数 > 此值判 NG
constexpr int   DONGBA_MIN_LEN_MM = 30;      // dongba 门槛(mm):最长边短于此值不计数(0=不过滤)
// 上面 30 = 2026-09-23 现场给的标准(「死节必须是大于 3 公分才算」), 界面上可改。
constexpr int   HEIBA_MAX_COUNT  = 24;       // heiba 小油疤:数量 > 此值判 NG(没有尺寸门槛)
// dongban 破洞原为「面积和占比 > 0.2%」。0.2% ≈ 100x100px ≈ 一个 55mm 的洞, 现在是
// 「可以有 2 个 30mm 的洞」—— 口径变了, 上线前得拿现场的板过一遍再定这两个数。
constexpr int   DONGBAN_MAX_COUNT  = 2;      // dongban 破洞:算数的块数 > 此值判 NG
constexpr int   DONGBAN_MIN_LEN_MM = 30;     // dongban 门槛(mm):最长边短于此值不计数
// quebian 缺边原为「面积和占比 > 0.5%」。0.5% ≈ 158x158px ≈ 一条 87mm 的缺边,
// 出厂值先跟破洞对齐(2/30), 同样不是等价换算, 上线前要过板。
constexpr int   QUEBIAN_MAX_COUNT  = 2;      // quebian 缺边:算数的块数 > 此值判 NG
constexpr int   QUEBIAN_MIN_LEN_MM = 30;     // quebian 门槛(mm):最长边短于此值不计数
// shupi/fabai: 2026-09-23 新接入的类, 现场没调过数 —— 数量给 99(远大于一块板上可能
// 出现的块数, 实际上等于不判), 门槛给现场标准 30。现场调好的数会写进 config.ini 覆盖。
constexpr int   SHUPI_MAX_COUNT  = 99;       // shupi 树皮:算数的块数 > 此值判 NG
constexpr int   SHUPI_MIN_LEN_MM = 30;       // shupi 门槛(mm):最长边短于此值不计数
constexpr int   FABAI_MAX_COUNT  = 99;       // fabai 发白:算数的块数 > 此值判 NG
constexpr int   FABAI_MIN_LEN_MM = 30;       // fabai 门槛(mm):最长边短于此值不计数
constexpr int   JIEBA_DONGBA_MAX_COUNT = 6;  // jieba+dongba 数量之和 > 此值判 NG
// 「dongban+quebian 面积之和占比 > 0.4%」这条组合规则 2026-09-23 现场决定【删掉】:
// 破洞/缺边都改走数量之后, 这条按面积算的跨类规则没有对应的口径了(个数和面积没法相加)。
// 单类的两条规则都在(各自按数量), 只是不再有这一条跨类的。原来的常量
// DONGBAN_QUEBIAN_AREA_RATIO 和 ini 键 dongban_quebian_area_pct 一起作废。

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

// 像素 → 毫米（木板所在的那张平面）。跟 measure.h 的 pixelToWorld 同一个口径：
// X_mm = (u - cx) * Z / fx，所以长度方向的换算系数就是 Z/fx，全图恒定。
// ⚠ 它只跟 DISTANCE_MM / FX 一样准：相机装高装低、镜头焦距不是标称值，这个数都跟着偏，
//   而 DONGBA_MIN_LEN_MM 这种「绝对毫米」门槛直接跟着偏（长度是线性，比面积好一点）。
constexpr float MM_PER_PX = DISTANCE_MM / FX;   // ≈ 0.5477 mm/px

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
