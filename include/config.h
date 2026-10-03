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
// 检测下限(全局): 低于这个分的检测【根本不存在】—— 不画框、不进统计、没机会参与判定。
// 跟下面 heiba 那一格(HEIBA_MIN_CONF)是同一层的东西, 只是一个管全体、一个管单个类。
// 2026-09-29 从 0.5 降到 0.3。依据就是下面 heiba 段里那条「新模型整体分数偏低」——
//   那件事当时只补了 heiba 一格, 别的类没补, 于是现场就出现「很多 dongba 之类的根本
//   没画框」(手工推理时它们的分数多在 0.3~0.4, 全被 0.5 丢掉了)。
//   这次直接动全局而不是再补四格: 受影响的是 7 个判定类里的 4 个, 一个个补就快等于全放了。
//   分工就此定下 —— 全局兜「模型整体分数偏低」, 按类的格只留给确实要单独处理的类。
// ⚠ 牵连: 判定类里【没有尺寸门槛替它挡误检】的只有 jieba 和 heiba。heiba 有自己更低的
//   0.25(不动), 所以真正会直接涨计数的是 jieba(> JIEBA_MAX_COUNT 判 NG, 现在 10)——
//   改完先盯它的数量。带尺寸门槛的那几个相对安全: 新冒出来的一般是小框, 会被 30mm 吸掉。
// ⚠ 引擎那道硬底(见下)现在得低于 0.3 才有效, 比原来只管 heiba 时更要紧。
constexpr float       CONF_THRESHOLD = 0.3f;

// ---- heiba 小油疤单独的检测下限: 2026-09-29 加的 ----
// 低于这个数的 heiba 检测【根本不算一次检测】(不画框、不进统计、没机会参与判定)。
// 它跟 CONF_THRESHOLD 是同一层的东西 —— 都是「有没有这次检测」, 只是在 process() 里
// 按类挑一个用: 别的类用全局 CONF_THRESHOLD, 只有 heiba 用这个更低的数。
// 为什么给 heiba 单独放低: 现场手工拿 .pt 直接跑推理, 看到很多真油疤的分数只有 0.28
//   左右 —— 按当时的全局 0.5 全被丢掉了。放低到 0.25 让这些能进来。
//   (顺带: 新模型整体分数都偏低, 验证图上 dongba/dongban/jieba/liefeng 也多在 0.3~0.4。
//    这句当时只是"顺带", 后来成了全局从 0.5 降到 0.3 的直接依据, 见上面 CONF_THRESHOLD。)
//
// ⚠⚠ 这一道跟 fabai 那道置信度门槛【不是一回事】, 别搞混:
//     这里是【检测下限】, 决定「存不存在」, 低于它的框连画都不画;
//     FABAI_MIN_CONF 是【计数门槛】, 决定「算不算数」, 不过的框照画、只是框线压暗。
//   所以这个数只可能比 CONF_THRESHOLD【低】(放宽); 想要更【严】的按类门槛,
//   得用 postprocessor 的 minConfFor 那道, 不是在这里填个大数。
//   下面的 static_assert 就是钉这件事。
//
// ⚠ 写死在这里、不上界面、不进 config.ini —— 现场明确要求(这个数是跟着模型走的,
//   不是工人该拧的旋钮)。要调就改这里重新编译。
// ⚠ 引擎导出时 EfficientNMS 里还烤了一个 score_threshold, 那是这道线【之前】的硬底:
//   那个数要是没低于 0.25, 这里填 0.25 也没用(框根本吐不出来) —— 换引擎时记得核一下。
// ⚠ heiba 是【唯一一道门槛都没有】的判定类(没尺寸门槛、也没计数门槛), 所以下限一放宽,
//   【0.25 及以上】的 heiba 全部计入 heiba > HEIBA_MAX_COUNT 那个数 —— 其中低于当时全局
//   (0.5) 的那一段是这次新多出来的(以前被 0.5 丢掉了), 0.5 以上本来就在算。
//   (全局后来又降到 0.3, 但 heiba 自己这一格没动 —— 它的计数口径从那天起就没再变过。)
//   现场 config.ini 里 heiba_max=24, 而新多出来的这一段模型给的框不少(手工推理时一块板上
//   七八个 heiba 都落在 0.3 一带) —— 上线前拿现场板过一遍, 24 这个数可能要放宽。
constexpr float HEIBA_MIN_CONF = 0.25f;

// 这个数只可能比全局低: 它的语义是「放宽」。要是填得比全局还高, 那就是想写一道更严的
// 门槛 —— 那种东西在 minConfFor(计数门槛) 那一层, 写在这儿会静默不生效(全局先把它筛了)。
static_assert(HEIBA_MIN_CONF <= CONF_THRESHOLD,
              "HEIBA_MIN_CONF 是【检测下限】, 只能比 CONF_THRESHOLD 低/相等; "
              "要更严的按类门槛请用 postprocessor.cpp 的 minConfFor");

// ---- 类别（下表的下标 = 模型的 class_id，必须与引擎训练时的 names 逐行一致） ----
// 现场叫法(界面/工人口头用的)与下面拼音类名的对应, 代码里只有拼音, 记这里免得回头认不出:
//   jieba   活节   —— 木节发白、按不掉, 不影响使用
//   dongba  死节   —— 节扣没掉, 但使劲一按就掉
//   heiba   小油疤 —— 黑色油滴在板面, 板子不碎; 单个没事, 数量多了才扔;
//                      **大油疤也标成这一类** —— 2026-10-03 从 dongban 挪过来的
//                      (在那之前它一直并进 dongban), 所以这个类现在有两道数量规则:
//                      「小油疤」+「大油疤」(见下面 HEIBA_BIG_*)
//   dongban 破洞   —— 节扣掉了板子被穿透, 底下黑传送带透出来;
//                      2026-10-02 起另有一道置信度门槛(见下面 DONGBAN_MIN_CONF);
//                      另有一道更严的数量规则「大破洞」(见下面 DONGBAN_BIG_*);
//                      (2026-10-03 之前这条也管大油疤, 那天挪走之后就不管了)
//   quebian 缺边
//   shupi   树皮   —— 板面带树皮; 2026-09-23 起按数量 + 尺寸门槛判(原为面积和占比),
//                      2026-09-30 起另有一道置信度门槛(见下面 SHUPI_MIN_CONF)
//   fabai   发白   —— 2026-09-23 新加的类, 按数量 + 尺寸门槛判, 另有置信度门槛
//                      (见下面 FABAI_MIN_CONF, 最早的一道); 待现场补一句这个类长什么样
// 其余(shuwen/piwenba/baowen/liefeng/heiban/banwen/banwenba)
// 目前只在图上画框显示, 不参与 NG 判定 —— 判定逻辑见 postprocessor.cpp 的 isNG()。
// 注意下面 SCRATCH_NG_LEN / SCRATCH_ASPECT 是给纹类(shuwen/piwenba/baowen)准备的,
// 常量定义了但 isNG() 里从来没实现, 现在全代码无引用。
//
// ⚠⚠ 顺序是这份文件里最要命的东西: 下标就是模型的 class_id, 顺序一错, 每个框都会被叫成
//   别的类、还会拿别的类的门槛去判它 —— 而且【不会报任何错】, 只是静默把结果判错。
//   这是本项目最容易出、最难查的一类事故, 2026-09-29 一天里出过【两次】(见下)。
//
// ⚠ 2026-09-29 这份顺序改过两次, 两次都是换引擎逼出来的:
//   第一次: 换成 models/train_npt640/ 那轮 —— 旧表是 dongba, dongban, jieba, shupi, ...;
//     新引擎是 dongban, dongba, heiba, jieba, ...(头两个对调、后面整体挪位), 而且 suibian
//     这个类在新数据集里没有了(模型输出 14 类, 旧表是 15 个名字)。当时的表现:
//       模型吐 0(破洞)  → 被叫成「死节」, 拿死节的门槛(2 个/30mm)去判它;
//       模型吐 10(发白) → 落到旧表的「suibian」上, 而 suibian 不参与判定 ⇒ 发白永不生效。
//   第二次(同一天晚些时候): 换成 models/train929_n960/ 那轮(960 输入) —— 那个引擎把 heiba
//     挪到了 0, shuwen/quebian、liefeng/shupi 各自对调。
//     ⚠ 这一次【不能】拿第一次的经验去推: 只有前 8 位在动, 尾巴(piwenba 往后)原封不动。
//     所以别"估着改", 一律照 confusion_matrix.png 逐行念。
//   第三次(2026-10-01): 换成 models/train1001_n960/ 那轮 —— 这次前 8 位是【整段重排】,
//     不是局部对调: dongban/dongba 各前移一位, heiba 从 0 掉到 2, jieba 从 3 甩到 7,
//     shuwen/liefeng/shupi 跟着前移; quebian 停在 5 没动, 那是巧合、不是规律。
//     尾巴(piwenba 往后)第三次原封不动。就是现在下面这个顺序。
//   ⚠ 三轮下来前 8 位已经来回翻过好几遍 —— 别再拿"上一轮长什么样"去推, 每轮都照那一轮
//     的 confusion_matrix.png 从头念一遍。
//
//   三次改的都只有这张表: 判定阈值、ini 键、界面行名全是【按名字】走的, 别处一处都不用动
//   (全项目按下标取类的地方只有这一张表 + postprocessor 里 cls_id→名字那一次转换)。
//
// 换引擎时怎么核对这张表(上线前务必做一遍, 五分钟的事):
//   训练目录里那张 confusion_matrix.png, 坐标轴顺序【就是】模型的 class_id 顺序
//   (Ultralytics 拿模型自己的 names 画的); models/labels.txt 应当是同一份。
//   把这两样对着下面这个表逐行念一遍, 有对不上的先别上线。
//   这份顺序的出处是 models/train1001_n960/(2026-10-01 第三轮训练) —— 照着那张矩阵和
//   labels.txt 核对出来的, 不是猜的。
//
// ⚠ 换引擎和改这张表必须【同时】上: 只换引擎不改表 = 每个框叫错名字、还拿错门槛去判它;
//   只改表不换引擎 = 一样错, 只是错的方向反过来。两个文件是绑在一起的一对。
const std::vector<std::string> CLASSES = {
    "dongban", "dongba", "heiba", "shuwen", "liefeng",
    "quebian", "shupi", "jieba", "piwenba", "baowen",
    "fabai", "heiban", "banwen", "banwenba"
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
//                 (dongba_min_len_mm / dongban_* / shupi_* / fabai_* / quebian_max_count /
//                  quebian_min_len_mm / dongban_big_* 是 2026-09-23 起新加的键, 现场
//                  还没填过, 没有 ini 时走下面的出厂默认值。heiba_big_* 是 2026-10-03
//                  加的(大油疤那条), 同上。dongban_area_pct /
//                  quebian_area_pct / dongban_quebian_area_pct / jieba_dongba_max 是作废
//                  的键, 代码里已经不读 —— ini 里还留着不影响任何东西, 不用去清)

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
// heiba 的【第二条】数量规则(2026-10-03 加的, 现场叫【大油疤】):
// 数的是最长边超过 HEIBA_BIG_MIN_LEN_MM 的 heiba 有多少块, 块数超过 HEIBA_BIG_MAX_COUNT
// 判 NG。形状跟下面 DONGBAN_BIG_* 那条「大破洞」完全一样 —— 同一个类的两道门槛, 一宽一严:
//   上面那条管「小的多」—— 允许 24 个小油疤;
//   这条管「单块太大」—— 一个 100mm 的油疤远比 24 个小的严重。
// 为什么现在才拆出来: 大油疤原来是并进 dongban 一起标的, 于是它跟着下面那条
//   「大破洞或大油疤」一起判。2026-10-03 标注挪到了 heiba, 那条规则就够不着它了
//   (它数的是 dongban), 所以给它单开一条 —— 界面上也从此是两行。
// ⚠ 两条规则各数各的: 大油疤这条数 heiba 里够大的那些, 小油疤那条照旧数【全部】heiba
//   (一个 100mm 的油疤既算进大油疤、也算进小油疤的 24)。这是 2026-10-03 定的 ——
//   跟以前 dongban 那一对(破洞 + 大破洞)的算法一致, 不悄悄改小油疤的计数。
// ⚠ 想让「1 个就判」得填 0(口径是「大于」, 0 = 超过 0 个 = 至少 1 个); 填 1 是「超过
//   1 个」= 两个才判。出厂给的是 1, 跟 DONGBAN_BIG_MAX_COUNT 对齐。
// ⚠ 这一个门槛管两处: 判定(数大油疤)和画框(够大的 heiba 框线换成黄色, 见 postprocessor
//   的 isBigHeiba)—— 同一个数, 不会出现「判定算它、图上不标它」这种对不上的情况。
constexpr int   HEIBA_BIG_MAX_COUNT  = 1;      // 大油疤:算数的块数 > 此值判 NG
constexpr int   HEIBA_BIG_MIN_LEN_MM = 100;    // 大油疤门槛(mm):最长边短于此值不计数
// dongban 破洞原为「面积和占比 > 0.2%」。0.2% ≈ 100x100px ≈ 一个 55mm 的洞, 现在是
// 「可以有 2 个 30mm 的洞」—— 口径变了, 上线前得拿现场的板过一遍再定这两个数。
constexpr int   DONGBAN_MAX_COUNT  = 2;      // dongban 破洞:算数的块数 > 此值判 NG
constexpr int   DONGBAN_MIN_LEN_MM = 30;     // dongban 门槛(mm):最长边短于此值不计数
// 破洞的【第二条】数量规则(2026-09-24 现场加的), 现场管它叫【一票否决】——
// 数的是「直径超过 DONGBAN_BIG_MIN_LEN_MM 的破洞」有多少块, 块数超过
// DONGBAN_BIG_MAX_COUNT 判 NG。
// 形状跟其他 5 个类完全一样 —— 数量 + 尺寸门槛, 界面同一套输入框, 没有单开一套新逻辑。
// 「一票否决」说的是它的【用意】不是它的【实现】: 一个 40mm 的大洞比两个 30mm 的严重,
// 一个就该拦下来, 而上面那条数块数的口径表达不出这件事(1 个 < 2 个, 反而放行)。
// 跟上面那条(2 个 30mm)是同一个类的两道门槛, 一宽一严:
//   上面那条管「小的多」—— 允许 2 个 30mm 的洞;
//   这条管「单块太大」—— 所以数量填得比上面那条小。
// ⚠ 想让「1 个就判」得把 DONGBAN_BIG_MAX_COUNT 填 0(口径是「大于」, 0 = 超过 0 个 = 至少
//   1 个), 填 1 是「超过 1 个」= 两个才判。出厂给的是 1。
// ⚠ 两个门槛各自独立, 但把 BIG_MIN_LEN 调到比 DONGBAN_MIN_LEN_MM 还小时, 这条会去数
//   一批「上面那条根本不算数」的小块 —— 逻辑上说得通(两条规则各看各的门槛), 但不是现场
//   那个意思(严格的那条反而更松)。界面上只挡得住 >500 这种越界, 挡不住这种大小关系。
// 界面上和 NG 原因串里这条规则叫「大破洞」—— 2026-10-03 之前叫「大破洞或大油疤」,
// 那天大油疤从 dongban 挪到了 heiba(见上面类名对照), 这条就只剩破洞了;
// 大油疤的那一半搬到了 HEIBA_BIG_*(见上面)。
constexpr int   DONGBAN_BIG_MAX_COUNT  = 1;   // 大破洞(一票否决):算数的块数 > 此值判 NG
constexpr int   DONGBAN_BIG_MIN_LEN_MM = 40;  // 大破洞(一票否决)门槛(mm):最长边短于此值不计数
// dongban 破洞的【置信度门槛】(2026-10-02 加的) —— 形状跟下面 SHUPI_MIN_CONF/FABAI_MIN_CONF
// 那两道一模一样(同一张表 postprocessor 的 minConfFor、同一处口径 countsTowardRule),
// 只是换了个类: 模型的 dongban 概率不高于这个值的, 那一块不算破洞。
// ⚠ 它只卡上面那条「破洞」(DONGBAN_MAX_COUNT): 那道门槛挂在 postprocessor 的
//   minConfFor 表上, 是 countsTowardRule 的一部分。
//   下面的「一票否决」(DONGBAN_BIG_MAX_COUNT)在 isNG 里是自己一条 if, 从来没接过这道
//   门槛 —— 一个 conf 0.35 的 40mm 洞不算破洞, 却照样能把板子否决掉。
//   (2026-10-03 核对代码时发现的: 加这道门槛那天(10-02)的注释写的是「两条都不数」,
//    跟代码对不上。这里改成照实说, 【没有】顺手改行为 —— 要让那条 if 也卡概率, 就是
//    给「一票否决」再加一道门槛, 会更严, 那是另一件事。)
// 0.45 是现场 2026-10-02 定的数 —— 它跟发白那个 0.65、树皮那个 0.40 不是一套换算关系,
// 别拿一个去推另一个, 也别在这条注释里替他们编一个理由。
// ⚠ 0 = 关掉这道门槛, 跟尺寸门槛的「0 = 不过滤」是同一个约定。
// ⚠ 有作用的区间是 0.31~0.99: 全局 CONF_THRESHOLD(0.3) 在更前面就把 0.3 以下的检测整个
//   丢掉了, 图上根本不会出现 conf < 0.3 的框, 填 0.3 及以下等于没填。界面上是
//   「破洞概率(置信度)」那个框(跟树皮/发白那两道一起排在发白那行下面, 一行一个),
//   口径也是【大于】(等于不算)。
constexpr float DONGBAN_MIN_CONF = 0.45f;
// quebian 缺边原为「面积和占比 > 0.5%」。0.5% ≈ 158x158px ≈ 一条 87mm 的缺边,
// 出厂值先跟破洞对齐(2/30), 同样不是等价换算, 上线前要过板。
constexpr int   QUEBIAN_MAX_COUNT  = 2;      // quebian 缺边:算数的块数 > 此值判 NG
constexpr int   QUEBIAN_MIN_LEN_MM = 30;     // quebian 门槛(mm):最长边短于此值不计数
// shupi/fabai: 2026-09-23 新接入的类, 现场没调过数 —— 数量给 99(远大于一块板上可能
// 出现的块数, 实际上等于不判), 门槛给现场标准 30。现场调好的数会写进 config.ini 覆盖。
constexpr int   SHUPI_MAX_COUNT  = 99;       // shupi 树皮:算数的块数 > 此值判 NG
constexpr int   SHUPI_MIN_LEN_MM = 30;       // shupi 门槛(mm):最长边短于此值不计数
// shupi 的【置信度门槛】(2026-09-30 加的) —— 形状跟下面 FABAI_MIN_CONF 那一段一模一样
// (同一张表 postprocessor 的 minConfFor、同一处口径 countsTowardRule), 只是换了个类:
// 模型的 shupi 概率不高于这个值的, 那一块不算树皮(也就进不了 SHUPI_MAX_COUNT)。
// 0.4 是现场 2026-09-30 定的数 —— 它跟发白那个 0.65 不是一套理由, 现场没说为什么是
// 0.4, 别在这条注释里替他们编一个。(这道门槛上线当天先给的是 0 = 关掉, 当天就按现场
// 的话改成了 0.40。)
// ⚠ 0 = 关掉这道门槛, 跟尺寸门槛的「0 = 不过滤」是同一个约定 —— 界面上填 0 就是不卡。
//    (这一档正好也起着「反悔开关」的作用: 树皮数量突然少了、怀疑是它卡的, 填 0 先排掉它。)
// 界面上是「树皮概率(置信度)」那个框(跟破洞/发白那两道一起排在发白那行下面, 一行一个),
// 口径也是【大于】(等于不算)。
constexpr float SHUPI_MIN_CONF   = 0.40f;
constexpr int   FABAI_MAX_COUNT  = 99;       // fabai 发白:算数的块数 > 此值判 NG
constexpr int   FABAI_MIN_LEN_MM = 30;       // fabai 门槛(mm):最长边短于此值不计数
// fabai 的【置信度门槛】(2026-09-29 加的) —— 本项目第一道非尺寸门槛
// (2026-09-30 起 shupi 也有了同样的一道, 见上面 SHUPI_MIN_CONF; 两张表都是 minConfFor)。
// 模型的 fabai 概率不高于这个值的, 那一块不算发白(也就进不了上面那个 FABAI_MAX_COUNT)。
// 为什么单给它开一道: 现场 2026-09-29 提的要求是「发白只有概率大于 0.65 才算真的是发白」
//   —— 也就是对发白这一个类再收紧一道, 别的类不动。做成 fabai 专属而不是去调全局
//   CONF_THRESHOLD, 是因为那个数一动就是 14 个类一起动, 而这里只针对发白。
//   (至于「为什么是发白」—— 现场没说原因, 别在这条注释里替他们编一个。)
// ⚠ 跟 MIN_LEN_MM 那类尺寸门槛【不是一回事】: 尺寸是从框上量出来、跟着相机标定走的,
//   置信度是模型自己给的分、跟标定无关。两道门槛是「且」的关系 —— 尺寸和概率都过了
//   才算数(见 postprocessor.cpp 的 countsTowardRule)。两个数谁松谁紧没有固定关系,
//   现场各自调: 把门槛调很高、数量上限留着 99, 效果就是「只有很确定的发白才拦板」。
// 界面「发白概率(置信度)」那个框认这个值, 口径是【大于】(等于不算), 跟界面文案一致。
constexpr float FABAI_MIN_CONF = 0.65f;
// 「dongban+quebian 面积之和占比 > 0.4%」这条跨类组合规则 2026-09-23 删掉: 破洞/缺边都
// 改走数量之后, 这条按面积算的没有对应的口径了(个数和面积没法相加)。常量
// DONGBAN_QUEBIAN_AREA_RATIO 和 ini 键 dongban_quebian_area_pct 一起作废。
// 「jieba+dongba 数量之和 > 6」这条跨类组合规则 2026-09-24 也删掉(现场定的)。常量
// JIEBA_DONGBA_MAX_COUNT 和 ini 键 jieba_dongba_max 一起作废。
// ⇒ 现在【一条跨类组合规则都没有了】: 判定只剩单类数量规则
//   (7 个类 + 两道更严的第二道门槛: 破洞的 DONGBAN_BIG_*「大破洞」和
//    小油疤的 HEIBA_BIG_*「大油疤」) + 板长/板宽。

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

// ---- 界面「退出」后的冷却时间: 2026-09-29 删掉 ----
// 原来界面有个「退出」按钮: 收到退出请求后 hide() 掉全屏窗口让桌面立刻可用, 但进程
// 不结束, 原地等 EXIT_RESTART_DELAY_SEC(150s) 再退出 —— 卡这段时间是为了让 systemd
// (Type=simple+Restart=always, 看进程还活着就不重启)别马上把全屏窗口拉回来, 好腾出
// 桌面开 RustDesk / 浏览器 / 改网络。
// 现在这一套整个删了, 换成界面上的「最小化」: 窗口直接藏起来, 屏幕角上留一个小条点
// 它回来, 进程照常跑(相机/PLC/推理都不断)。同一个目的(别让全屏软件占着桌面), 但
// 不用停检测、也不用等 150 秒、更不用靠 systemd 拉起。
// ⇒ 常量和那圈逐秒倒数一起作废; 现在进程只在收到 SIGINT/SIGTERM 时退出, 由 systemd 的
//   RestartSec(3s) 管恢复, 崩溃和主动重启的恢复时间一致。

// ---- 输出 ----
constexpr bool  SAVE_IMAGES   = true;
constexpr bool  SHOW_DISPLAY  = true;
constexpr const char* OUTPUT_DIR = "./output/";
constexpr const char* SAVE_ENABLE_PASSWORD = "629785";   // 界面开启「存图开关」所需密码

// 今日统计（界面那个合格率）的落盘文件，相对 OUTPUT_DIR。内容是一行
// "YYYY-MM-DD <总数> <NG数>" —— 纯文本，现场 cat 一下就能看，想手动清当天数据直接删文件。
// 放在 output/ 根目录（不是 raw/ 或 result/）：cleanup_images.sh 只管那两个子目录里的
// *.jpg，这个文件它碰不到，不会被清图连带删掉。细节见 main.cpp 的 loadDailyStats。
constexpr const char* DAILY_STATS_FILE = "daily_stats.txt";

// OK 板抽样比例(%)：开发者模式【没开】时按这个数抽 OK 板，0 = 不抽。
// 不依赖开发者模式(密码)也能留下 OK 板 —— 查漏检要拿 OK 板当反例看。
// 只管 OK 板：NG 板一直是无条件全存(原始图+结果图)，不看这个数。
// ⚠ 这一个数同时喂给原始图/结果图两条抽样，是【故意的】：抽中的板要两张图一起落盘
//   （两张共用同一个 board_id，成对才说得清是哪块板）。比例同源，两条抽样才会同进同出、
//   永远落在同一块板上。要两张分别调比例，得改成「一次决定 + 两个 push」，
//   别在这里拆成两个常数 —— 拆开就成了两张图各抽各的，会出现有原始图没结果图。
// 采样实现是「每 N 块存第 N 块」(N = 100/pct 向下取整)，所以只有 100 的约数才是精确
// 比例：10 → 1/10 ✓，15 → 100/15=6 → 实际 1/6。
// 开发者模式开着时以界面上那两行为准（工程师现场调，不持久化）。
constexpr int   OK_SAVE_PCT = 10;

} // namespace Config
