#include "postprocessor.h"
#include "config.h"
#include <algorithm>
#include <cmath>
#include <sstream>
#include <iomanip>

// 检测框最长边 → 毫米。所有尺寸门槛（死节/破洞/缺边/树皮/发白）共用这一个口径，
// 详见 countsTowardRule。
// 用最长边而不是对角线：现场说法是「大于 3 公分」，对角线会把这个数放大 √2 倍左右（方框），
// 细长框更是放大到接近长边本身 —— 门槛跟着框形变，没法跟工人解释。
// ⚠ 2026-10-03 起有【一处例外】：大油疤(heiba 的第二道规则)改用量对角线，见下面的
//   boxDiagonalMm。它跟这里说的理由不冲突 —— 那次是现场拿油疤的形状(长条/方形都有)
//   单独定的，只改了那一条门槛，别的类照旧走这个函数。
// ⚠ MM_PER_PX 是标定换算（config.h），相机装高装低、镜头焦距不是标称值它就偏 ——
//   所以这些「绝对毫米」门槛天生带标定误差，跟以前的面积占比那种纯像素比值不是一回事。
static float boxLongSideMm(const cv::Rect& box) {
    return (float)std::max(box.width, box.height) * Config::MM_PER_PX;
}

// 检测框的对角线(mm) = √(宽² + 高²) × 标定值。
// ⚠ 全项目只有【大油疤】一条规则用它(见 isBigHeiba), 别顺手拿它替掉别的类的
//   boxLongSideMm —— 两者量出来的数不一样(方形框差 ×√2), 换了就是把那道门槛悄悄改了。
// 为什么大油疤要用这个: 见 config.h 的 HEIBA_BIG_MIN_DIAG_MM 那段(长条/方形都有,
// 最长边会跟着缺陷朝向变)。
static float boxDiagonalMm(const cv::Rect& box) {
    const float w = (float)box.width  * Config::MM_PER_PX;
    const float h = (float)box.height * Config::MM_PER_PX;
    return std::sqrt(w * w + h * h);
}

// 「这个类量尺寸用哪把尺子」。只有 heiba 用对角线，别的类(含 dongban 的「大破洞」)都是最长边。
// ⚠ 这是【唯一】一处决定量法的地方，两条读它的路都在下面：
//     小油疤(heibaMaxCount) 走 countsTowardRule → 这里；
//     大油疤(heibaBigMaxCount) 走 isBigHeiba → 这里。
//   所以 heiba 这两条规则【共用一把尺子】—— 不会出现同一个类两条规则量法不一致。
//   要改成别的类也用量对角线，就在这一行加个类名，别去 isNG / draw 里各写一份条件。
// 为什么 heiba 用对角线：见 config.h 的 HEIBA_BIG_MIN_DIAG_MM 那段(长条/方形都有，
// 最长边量出来的数会跟着缺陷朝向变)。
// ⚠ 界面上【不提】对角线(2026-10-03 现场定的)：小油疤和大油疤两行的输入框后缀都照
//   通用的 " mm" 写。别看见界面没写就以为是漏了，是特意拿掉的。
static bool gateUsesDiagonal(const std::string& name) {
    return name == "heiba";
}

// 某个类的「大小」→ 毫米。走的就是上面那把尺子。
// 尺寸门槛(countsTowardRule)、大油疤(isBigHeiba)都从这里取，谁都不许自己挑函数。
static float boxSizeMm(const std::string& name, const cv::Rect& box) {
    return gateUsesDiagonal(name) ? boxDiagonalMm(box) : boxLongSideMm(box);
}

// 每个类的框颜色 (BGR)。draw() 画框 和 drawSummary() 面板文字 共用，保持颜色一致
static cv::Scalar classColor(const std::string& name) {
    if (name == "dongba")       return cv::Scalar(0, 120, 255);   // 深橙
    if (name == "dongban")      return cv::Scalar(160, 90, 0);    // 深青
    if (name == "jieba")        return cv::Scalar(255, 255, 0);   // 青
    if (name == "shupi")        return cv::Scalar(128, 128, 128); // 灰
    if (name == "shuwen")       return cv::Scalar(0, 165, 255);   // 橙
    if (name == "heiba")        return cv::Scalar(0, 0, 255);     // 红
    if (name == "piwenba")      return cv::Scalar(255, 0, 255);   // 品红
    if (name == "quebian")      return cv::Scalar(0, 0, 128);     // 深红
    if (name == "baowen")       return cv::Scalar(255, 165, 0);   // 蓝? 实际是BGR
    if (name == "liefeng")      return cv::Scalar(0, 0, 200);     // 深红
    // 这里原来还有一行 suibian(深黄) —— 2026-09-29 新引擎里没有这个类了(config.h 的
    // CLASSES 已去掉), 那一行再也不会被走到, 一并删掉。以后真拿回这个类, 记得给它补个色。
    if (name == "heiban")       return cv::Scalar(255, 0, 0);     // 蓝
    if (name == "banwen")       return cv::Scalar(255, 0, 128);   // 紫
    if (name == "banwenba")     return cv::Scalar(200, 0, 200);   // 浅紫
    return cv::Scalar(0, 255, 0);                                 // 默认绿
}

// 大油疤（够大的 heiba）的框线颜色：黄。
// 为什么单独给一个色：大油疤和小油疤是【同一个类名】(heiba)，模型分不开，只有大小分得开，
// 而判定那边单列了一条更严的规则专管够大的那些（见 isNG 里的 heiba_big_cnt）。
// 判定分了、图上不分的话，工人看到一块 120mm 的油疤画着跟小油疤一样的红框，
// 是没法知道它归哪条规则管的。
// 挑黄是因为 classColor 那张表里没被任何类占用（唯一那处黄是左边统计面板的标题行，
// 那是面板自己的字色，不是某个类的框色）。门槛跟判定共用 _heiba_big_min_diag_mm 一个数
// （见 postprocessor.h 的 isBigHeiba），不会出现「判定算它、图上是别的颜色」。
static const cv::Scalar HEIBA_BIG_COLOR = cv::Scalar(0, 255, 255);   // 黄

// 「这一块不算数」的统一画法：把类色整体压暗，而不是换成另一个颜色。
// 为什么不换色：灰色已经被 shupi 占了，别的颜色又各有其主，换色会让工人以为框里
// 是另一个类。压暗则一眼看出「还是这个类，只是不算数」。
// 不算数的原因不只有「太小」：fabai/shupi 还有一道置信度门槛（见 countsTowardRule），
// 概率不够的也这样压暗。压暗不区分原因，工人要的是「算不算数」这一个答案。
static cv::Scalar dimColor(const cv::Scalar& c) {
    const double k = 0.45;   // 压到 45%：暗到能区分，又不至于在深色板上看不见
    return cv::Scalar(c[0] * k, c[1] * k, c[2] * k);
}

// 检测下限表: 哪个类的下限跟全局不一样。没列进来的类 = 用全局 CONF_THRESHOLD。
// 现在只有 heiba 一格(比全局【低】, 放宽), 为什么见 config.h 的 HEIBA_MIN_CONF。
// ⚠ 这是「存不存在」那一层, 不是「算不算数」那一层: 低于它的框连画都不画, 跟
//   sizeGateMm/minConfFor 那种「照画、压暗、只是不算数」是两回事。所以这张表里只可能
//   出现比全局【小】的数 —— 要比全局严的按类门槛请去 minConfFor, 写这儿会静默不生效。
float Postprocessor::minDetectConf(const std::string& name) const {
    if (name == "heiba") return Config::HEIBA_MIN_CONF;
    return _thresh;
}

std::vector<Defect> Postprocessor::process(const trtyolo::DetectRes& res,
                                            cv::Mat& frame, const cv::Size& size) {
    std::vector<Defect> defects;

    for (int i = 0; i < res.num; ++i) {
        // 先把类名解出来 —— 下面挑检测下限要用它, 而门槛是【按类】的。
        const int cls_id = res.classes[i];
        const std::string name = (cls_id < (int)_classes.size()) ? _classes[cls_id] : "?";

        // 检测下限(minDetectConf)：低于它的检测【根本不存在】—— 不画框、不进统计、
        // 也没机会参与判定，跟「小到不算数」(框照画、只是压暗)完全是两种处理。
        // 绝大多数类就是全局 CONF_THRESHOLD，只有 heiba 用自己那个更低的数
        // (config.h HEIBA_MIN_CONF, 为了把手工推理时看到的 0.28 那一档接住)。
        // ⚠ 别跟 fabai/shupi 那道置信度门槛搞混：那道在这一步【之后】(见 countsTowardRule)，
        //   是「这次检测算不算数」；这道是「这算不算一次检测」。
        // ⚠ 引擎导出时 EfficientNMS 里还烤着一个 score_threshold，那是这道线【之前】的
        //   硬底 —— 那个数要是没低于 0.25，这里填多少都没用（框根本吐不出来）。
        if (res.scores[i] < minDetectConf(name)) continue;

        const auto& b = res.boxes[i];
        int x = std::max(0, (int)b.left);
        int y = std::max(0, (int)b.top);
        int w = std::min((int)(b.right - b.left), size.width - x);
        int h = std::min((int)(b.bottom - b.top), size.height - y);

        Defect d;
        d.cls_id = cls_id;
        d.conf   = res.scores[i];
        d.box    = cv::Rect(x, y, w, h);
        d.name   = name;
        defects.push_back(d);
    }

    draw(frame, defects);
    drawSummary(frame, defects);   // 左上角统计面板
    return defects;
}

// ============================================================
// NG 判定 —— 全流程唯一决定 OK/NG 的地方
//
// 判据现在只有一种: 【数量】。7 个类各有一个数量上限, 超了判 NG
// (另外破洞和 heiba 各有第二道更严的, 见下面那两段, 所以一共 9 条规则):
//   jieba 活节 / heiba 小油疤 —— 没有尺寸门槛, 进来了的全算进数量。
//       (heiba 另有一道【检测下限】, 见 process() 里的 minDetectConf: 它比全局低,
//        0.25 以上就放进来, 所以 0.25 到全局那一档也算它的数量。jieba 没有, 就是全局 ——
//        2026-09-29 全局从 0.5 降到 0.3 之后, jieba 直接把 0.3~0.5 这一段也收进计数,
//        它是这一改最直接受影响的类, 盯 JIEBA_MAX_COUNT。)
//   dongba 死节 / dongban 破洞 / quebian 缺边 / shupi 树皮 / fabai 发白 ——
//       数之前先过一道【尺寸门槛】: 检测框最长边换算成毫米, 短于门槛的不计数
//       (工人界面填, 0=不过滤)。这是【过滤】不是【拒绝】: 小的不会把板子判死,
//       只是不算进它那一类的计数。
//       门槛值各类各管各的(死节 30mm 是现场标准, 其余现场还没调过)。
//       判定口径只有 countsTowardRule() 一处, draw() 用的也是它(只不过把框线压暗来画)——
//       两处必须同一个口径, 否则会出现「画成不算数的颜色、却被数进去判了 NG」。
//   fabai 发白 / shupi 树皮 / dongban 破洞 另有一道【置信度门槛】(fabai 那道 2026-09-29
//       加的, shupi 那道 2026-09-30 加的, dongban 那道 2026-10-02 加的; 界面行名
//       「破洞概率(置信度)」/「树皮概率(置信度)」/「发白概率(置信度)」):
//       模型给的概率不高于这个值的, 那一块不算这个类。它跟尺寸门槛并列在
//       countsTowardRule() 里(两道是「且」), 所以判定/框色/统计三处照样只有一个口径 ——
//       这是把它放进 countsTowardRule 而不是在 isNG 里另写一条 if 的理由。
//       跟 dongban 那道第二门槛(走自己的 if)不同: 那条是同一个类的第二道【尺寸】门槛,
//       一张表一类只能放一个数; 这条是【另一种】门槛, 表里放得下, 所以归表。
//       (dongban 两样都有 —— 第二道尺寸门槛走自己的 if、置信度门槛归这张表 ——
//        一个是尺寸一个是概率, 不冲突, 也不违反「一张表一类一个数」。)
//       为什么最早给发白开这道、树皮和破洞又是怎么跟上来的 —— 见 config.h 的
//       FABAI_MIN_CONF / SHUPI_MIN_CONF / DONGBAN_MIN_CONF。
//   dongban 破洞和 heiba 小油疤各有【第二道】同样形状、门槛更严的规则(现场叫「一票否决」,
//       界面行名/原因串里是「大破洞」和「大油疤」): 数的是「最长边超过各自 big 门槛」的
//       块数。它们不走 sizeGateMm/countsTowardRule(那是每类一处的门槛表), 各自一条 if
//       —— 一个类两道门槛, 表里放不下第二道。
//       「一票否决」是这个规则的【用意】不是【实现】: 它仍然是一条数量规则(形状跟上面
//       那条一模一样), 只是数量和门槛都配得更严, 让一块够大的缺陷自己就能把板子拦下来。
//       ⚠ 大油疤那条是 2026-10-03 才有的: 在那之前大油疤并进 dongban 一起标, 跟着「大破洞」
//         那条走; 那天它的标注挪到了 heiba, 于是 heiba 这边也需要一条够不着它就没法判的规则。
//         两条【各数各的】: 一个够大的油疤既算进小油疤那条(24), 也算进大油疤这条 ——
//         跟 dongban 那一对(破洞 + 大破洞)一直是同一个算法, 不是漏了去重。
//
// 为什么全改成数量(2026-09-23 一天里改完的):
//   原来 dongban/quebian/shupi/fabai 走的是【面积和占整图比例】。那个口径的根本毛病是
//   它把「一堆小的」和「一个大的」算成同一个数 —— 30 个小缺陷的面积和可以大于 1 个大
//   缺陷, 但这两块板该不该扔正好相反。尺寸门槛 + 数量才分得开, 而且门槛是工人听得懂的
//   说法(「大于 3 公分的才算」), 面积占比则是个谁也没法目测的数。
//   注: 面积算的是检测框 w*h, 不是真实缺陷像素面积(圆形框比实际大约 1.27 倍), 这也是面积
//   口径不好用的一面。换成最长边之后没有这个膨胀 —— 最长边本来就是框上直接量的。
//
// 跨类组合规则曾经有两条, 现在【一条都没有了】:
//   「破洞+缺边 面积之和占比 > 0.4%」 2026-09-23 删 —— 破洞/缺边都改走数量之后, 这条
//     按面积算的凑不出合并口径(个数和面积没法相加);
//   「活节+死节 数量之和 > 6」        2026-09-24 删(现场定的)。
// 两条的常量/ini 键都作废了, 见 config.h。现在剩下的全是单类规则。
//
// 其余类(shuwen/piwenba/baowen/liefeng/heiban/banwen/banwenba)
// 一律默认 OK, 只在左上角面板画框显示。要接入就: 这里加分支 + postprocessor.h 加阈值成员
// + 界面加输入框 + main.cpp 下发, 四处都要动。
// (背景: 模型 mAP50≈0.49 偏低, 未经现场验证的类直接参与判定会大量误杀。)
//
// 阈值全部运行时可调, 由界面「工人设置」输入框经 main.cpp 每板下发(见 postprocessor.h)。
// ============================================================

// 尺寸门槛表: 哪个类有门槛、门槛多少毫米。没列进来的类 = 没门槛 = 全都算。
// 加一道新门槛就在这里加一行, 别去 isNG / draw 里各写一份条件 —— 那正是这个函数存在的理由。
int Postprocessor::sizeGateMm(const std::string& name) const {
    if (name == "dongba")  return _dongba_min_len_mm;
    if (name == "dongban") return _dongban_min_len_mm;
    if (name == "heiba")   return _heiba_min_diag_mm;   // 2026-10-03 起 heiba 也有门槛了
    if (name == "quebian") return _quebian_min_len_mm;
    if (name == "shupi")   return _shupi_min_len_mm;
    if (name == "fabai")   return _fabai_min_len_mm;
    return 0;
}

// 置信度门槛表: 哪个类要求「模型给的把握大于多少」才算数。没列进来的类 = 没这道门槛。
// 跟上面那张尺寸门槛表是一对: sizeGateMm 管「多大才算」, 这个管「多确定才算」。
// 现在有三道: fabai(这道是最早的) / shupi(2026-09-30 加的同一道) / dongban(2026-10-02
// 加的同一道), 形状照抄上面那张表 —— 一是以后再加类不必另想写法, 二是让 countsTowardRule
// 里那句「怎么算过门槛只留这一处」继续成立(表里加一行就够了, 不用去 isNG/draw/drawSummary
// 各补一个条件)。
// 顺序照 CLASSES 来(dongban 在最前, 跟 config.h 里那几个常量的排列一致), 不是随手排的。
double Postprocessor::minConfFor(const std::string& name) const {
    if (name == "dongban") return _dongban_min_conf;
    if (name == "fabai")   return _fabai_min_conf;
    if (name == "shupi")   return _shupi_min_conf;
    return 0.0;
}

// 这个类有没有【任何】门槛 —— 尺寸的或置信度的。目前只有 drawSummary 用它挑写法,
// 但放在这里(而不是在那个函数里 or 两个表)是因为「有没有门槛」是门槛表自己的事。
bool Postprocessor::hasGate(const std::string& name) const {
    return sizeGateMm(name) > 0 || minConfFor(name) > 0.0;
}

bool Postprocessor::countsTowardRule(const Defect& d) const {
    // 置信度门槛(目前有 fabai/shupi 两道): 模型自己给的把握不够, 这块就不算数。
    // 口径是【大于】——`<=` 判为不算, 跟界面上写的「发白概率(置信度) >0.65」逐字对应,
    // 也跟下面尺寸门槛的 `> gate` 一个方向(那边也是「大于才算」)。
    // 两道门槛是「且」: 这里先筛置信度, 过了再去看尺寸, 任何一道没过都返回 false。
    const double conf_gate = minConfFor(d.name);
    if (conf_gate > 0.0 && (double)d.conf <= conf_gate) return false;

    const int gate = sizeGateMm(d.name);
    if (gate <= 0) return true;                                   // 没门槛 / 门槛关掉
    // 量法跟着类走（heiba 是对角线，别的类是最长边），见 boxSizeMm。
    return boxSizeMm(d.name, d.box) > (float)gate;
}

// 「这块 heiba 够不够大、算不算大油疤」。判定和画框都走这一处，理由同 countsTowardRule。
// ⚠ 它【不】是 countsTowardRule 的一部分：大油疤不是「不算数」，是「算到另一条规则里去」
//   （小油疤那条数全部 heiba，大油疤这条数够大的那些，两条各数各的）。所以这里返回 true
//   的框不会因此被压暗，只是框线换成黄色。
// ⚠ 量的是【对角线】(boxDiagonalMm)，全项目唯一一处 —— 别的尺寸门槛都是最长边，
//   为什么这里例外见 config.h 的 HEIBA_BIG_MIN_DIAG_MM。
bool Postprocessor::isBigHeiba(const Defect& d) const {
    if (d.name != "heiba") return false;
    if (_heiba_big_min_diag_mm <= 0) return false;   // 门槛 0 = 这条规则关掉
    // 走 boxSizeMm(heiba → 对角线)，跟小油疤那条同一把尺子。
    return boxSizeMm(d.name, d.box) > (float)_heiba_big_min_diag_mm;
}

bool Postprocessor::isNG(const std::vector<Defect>& defects,
                         float len_mm, float wid_mm, std::string& reason,
                         bool* size_only) const {
    int jieba_cnt   = 0;
    int dongba_cnt  = 0;      // 下面这几个都只数「门槛全过了」的那些，见 countsTowardRule
                              // （dongba/dongban/quebian/shupi 是尺寸门槛，shupi/fabai 还要加一道置信度）
    int dongban_cnt = 0;
    int quebian_cnt = 0;
    int heiba_cnt   = 0;
    int shupi_cnt   = 0;
    int fabai_cnt   = 0;
    int dongban_big_cnt = 0;  // 大破洞：_dongban_big_min_len_mm 以上的破洞块数
    int heiba_big_cnt   = 0;  // 大油疤：对角线超过 _heiba_big_min_diag_mm 的 heiba 块数

    for (const auto& d : defects) {
        // 没门槛的类直接数；有门槛的类先问 countsTowardRule。门槛只管计数，
        // 框该画还画（draw 把不过门槛的框线压暗）。
        if (d.name == "jieba") {
            jieba_cnt++;
        } else if (d.name == "heiba") {
            // 小油疤：过门槛的才算数（门槛是对角线，2026-10-03 加的，见 HEIBA_MIN_DIAG_MM）。
            // 够大的那些【同时】在下面那条「大油疤」里再数一次 —— 两条各数各的，
            // 跟 dongban 那一对（破洞 + 大破洞）一个算法。
            // ⚠ 默认 10mm 的门槛 < 大油疤的 100mm，所以过了大油疤的必然也过小油疤，
            //   「两条都数」这个性质是门槛数值保证的。要是哪天把 _heiba_min_diag_mm 调到
            //   比大油疤还大，中间那一档（大油疤门槛以上、小油疤门槛以下）就会只算进大油疤
            //   —— 那是配置出来的行为，不是漏了，但现场大概不会这么填。
            if (countsTowardRule(d)) heiba_cnt++;
            if (isBigHeiba(d)) heiba_big_cnt++;
        } else if (d.name == "dongba") {
            if (countsTowardRule(d)) dongba_cnt++;
        } else if (d.name == "dongban") {
            if (countsTowardRule(d)) dongban_cnt++;
            // 第二道门槛(大破洞)跟上面那道各看各的, 不走 sizeGateMm/countsTowardRule
            // —— 一个类两道门槛, 那张表一类只能放一个数。超过 BIG_MIN 的块必然也过了
            // 上面那道(40 > 30), 但这里不依赖这个大小关系, 两个门槛都能单独调。
            if (_dongban_big_min_len_mm > 0 &&
                boxLongSideMm(d.box) > (float)_dongban_big_min_len_mm)
                dongban_big_cnt++;
        } else if (d.name == "quebian") {
            if (countsTowardRule(d)) quebian_cnt++;
        } else if (d.name == "shupi") {
            if (countsTowardRule(d)) shupi_cnt++;
        } else if (d.name == "fabai") {
            if (countsTowardRule(d)) fabai_cnt++;
        }
        // 其余类(shuwen/piwenba/baowen/liefeng/heiban/banwen/banwenba)
        // 默认 OK，不判 NG，只在左上角面板画框显示
    }

    std::vector<std::string> reasons;
    // 原因串拼法：带门槛的类必须把门槛写进去，否则工人看到「死节>2」会去数图上所有该类框
    // （包括不算数的小块），数出来比 2 多，以为程序数错了。置信度门槛同理：看到「发白>99」
    // 去数图上所有发白框，其中一部分是因为概率不够没算数，不写清楚又是一次「程序数错了」。
    // （树皮 2026-09-30 也多了这道，理由完全一样。）
    // gate_mm / gate_conf 传 0 就是不写那一道门槛（现在 7 个类里只有 jieba 两道都没有）。
    // 两道都有时合成一个括号（"发白>99(30mm以上,概率>0.65)"），不叠两层括号。
    auto countReason = [](const char* cn, int max_cnt, int gate_mm, double gate_conf = 0.0) {
        std::string s = std::string(cn) + ">" + std::to_string(max_cnt);
        std::string gate;
        if (gate_mm > 0) gate = std::to_string(gate_mm) + "mm以上";
        if (gate_conf > 0.0) {
            std::ostringstream cs;
            cs << "概率>" << std::fixed << std::setprecision(2) << gate_conf;
            if (!gate.empty()) gate += ",";
            gate += cs.str();
        }
        if (!gate.empty()) s += "(" + gate + ")";
        return s;
    };
    // 顺序沿用改口径之前那一版（活节/死节/小油疤/破洞/缺边/树皮/发白），
    // 别按分类重排 —— 现场是照着这一串的字面顺序在看板的。
    // 两道「一票否决」各跟着自己的主规则排：大破洞跟破洞、大油疤跟在大破洞后面
    // —— 界面上那两行也正是这么紧挨着排的（见 mainwindow.cpp 的工人设置）。
    if (jieba_cnt > _jieba_max_count)
        reasons.push_back(countReason("活节", _jieba_max_count, 0));
    if (dongba_cnt > _dongba_max_count)
        reasons.push_back(countReason("死节", _dongba_max_count, _dongba_min_len_mm));
    // 小油疤 2026-10-03 起也有尺寸门槛了（以前这里传 0 = 不写门槛）。原因串照通用格式，
    // 不写「对角线」（量法不外露，见 mainwindow.cpp 那行注释）。
    if (heiba_cnt > _heiba_max_count)
        reasons.push_back(countReason("小油疤", _heiba_max_count, _heiba_min_diag_mm));
    if (dongban_cnt > _dongban_max_count)
        reasons.push_back(countReason("破洞", _dongban_max_count, _dongban_min_len_mm));
    // 破洞的第二道(现场叫「一票否决」)。跟上面那条是同一个类的两条规则, 原因串必须让
    // 人分得清是哪一条 —— 所以写「大破洞」, 跟上面那条「破洞」是两个不同的字符串,
    // 工人照着界面上的行名一对就知道是哪行触发的。
    // (2026-10-03 之前这个串是「大破洞或大油疤」: 那时大油疤也标成 dongban, 这条一并
    //  管着它。那天大油疤的标注挪到了 heiba, 这条就只剩破洞 —— 大油疤去了下面那条。)
    if (dongban_big_cnt > _dongban_big_max_count)
        reasons.push_back(countReason("大破洞", _dongban_big_max_count,
                                      _dongban_big_min_len_mm));
    // 大油疤 —— heiba 那边的第二道(2026-10-03 加的), 形状跟上面那条大破洞一模一样,
    // 只是换了个类: 数 heiba 里够大的那些。紧挨着大破洞排, 因为这俩在界面上也是挨着的
    // 两行, 而且都是「一块太大就否决」这层意思。
    // 原因串照通用格式走（「大油疤>1(100mm以上)」），不写「对角线」—— 量法是内部的事，
    // 界面上不提（2026-10-03 现场定的，见 mainwindow.cpp 那一行的注释）。
    if (heiba_big_cnt > _heiba_big_max_count)
        reasons.push_back(countReason("大油疤", _heiba_big_max_count,
                                      _heiba_big_min_diag_mm));
    if (quebian_cnt > _quebian_max_count)
        reasons.push_back(countReason("缺边", _quebian_max_count, _quebian_min_len_mm));
    if (shupi_cnt > _shupi_max_count)
        reasons.push_back(countReason("树皮", _shupi_max_count, _shupi_min_len_mm,
                                      _shupi_min_conf));
    if (fabai_cnt > _fabai_max_count)
        reasons.push_back(countReason("发白", _fabai_max_count, _fabai_min_len_mm,
                                      _fabai_min_conf));
    // 这里原来还有一条跨类组合规则（活节+死节 数量之和 > 6），2026-09-24 现场删了。
    // 现在 reasons 里全是单类规则，一条跨类的都没有（见函数头那段）。
    // ⚠ 常量和 ini 键（JIEBA_DONGBA_MAX_COUNT / jieba_dongba_max）都作废了，已删；
    //   ini 里那个键还留着不影响，不用去清。

    // 尺寸规则跟上面那 7 条缺陷规则不是一回事，得分开数：存图那边只给「有真缺陷」的
    // NG 留档，纯尺寸 NG（板小了一点）不存。所以拼尺寸原因之前先把缺陷原因的条数记下来。
    const size_t n_defect = reasons.size();

    // 木板尺寸判定：测得长/宽低于阈值判 NG（0=没测到，不判尺寸）
    if (len_mm > 0 && len_mm < _min_length_mm)
        reasons.push_back("板长<" + std::to_string(_min_length_mm) + "mm");
    if (wid_mm > 0 && wid_mm < _min_width_mm)
        reasons.push_back("板宽<" + std::to_string(_min_width_mm) + "mm");

    reason.clear();
    for (size_t i = 0; i < reasons.size(); ++i) {
        if (i) reason += " ";
        reason += reasons[i];
    }
    // 只有尺寸原因：缺陷原因一条都没有，而确实多了尺寸原因
    if (size_only) *size_only = reasons.size() > n_defect && n_defect == 0;
    return !reason.empty();
}

void Postprocessor::draw(cv::Mat& frame, const std::vector<Defect>& defects) {
    for (const auto& d : defects) {
        // 没过门槛的缺陷照样画框，但框线压暗一档 —— 一眼分出「这个不算数」。
        // 这里说的门槛是【全部】门槛（尺寸 + 置信度都算），判断走 countsTowardRule，
        // 跟 isNG 数个数时同一个口径，颜色和结论不会打架。
        // 压暗不区分是哪道门槛卡的：工人要的是「算不算数」这一个答案。想知道是被哪道卡的，
        // 看框上那个标签 —— 概率就印在上面（fabai 0.58），跟界面「发白概率(置信度)」一比就明白。
        // ⚠ 标签底色【不】跟着压暗：现场要求标签保持一致（一眼看清是哪个类），
        //   所以算不算数只体现在框线上。
        const cv::Scalar cls  = classColor(d.name);
        // 大油疤（够大的 heiba）框线换黄 —— 它跟小油疤是同一个类名，模型分不开，
        // 只有大小分得开，而判定那边有一条规则专管够大的那些（见 isNG 的 heiba_big_cnt）。
        // 只换框线、不动标签底色：标签是「这是哪个类」的答案，底色跟着变反而认不出是 heiba。
        // 够不够大走 isBigHeiba，跟判定同一个门槛，颜色和结论不会打架。
        const cv::Scalar line_c = isBigHeiba(d) ? HEIBA_BIG_COLOR : cls;
        const cv::Scalar box_c  = countsTowardRule(d) ? line_c : dimColor(line_c);

        cv::rectangle(frame, d.box.tl(), d.box.br(), box_c, 2);

        std::ostringstream ss;
        ss << d.name << " " << std::fixed << std::setprecision(2) << d.conf;
        int bl;
        auto ts = cv::getTextSize(ss.str(), cv::FONT_HERSHEY_SIMPLEX, 0.5, 1, &bl);
        cv::rectangle(frame,
            cv::Point(d.box.x, d.box.y - ts.height - 6),
            cv::Point(d.box.x + ts.width + 4, d.box.y), cls, cv::FILLED);
        cv::putText(frame, ss.str(),
            cv::Point(d.box.x + 2, d.box.y - 4),
            cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(255, 255, 255), 1);
    }
}

// 左上角统计面板：类别数 + 各类框数
void Postprocessor::drawSummary(cv::Mat& frame, const std::vector<Defect>& defects) {
    if (frame.empty()) return;

    // 按类别统计两个数：
    //   cnt   —— 画出来的框数（没过门槛的那些也在内：工人是对着图数的，
    //            图上画了几个框，面板就该能对上几个）
    //   gated —— 其中把门槛全过了的（尺寸 + 置信度，fabai/shupi 两道都有），
    //            也就是 isNG 真正数进去的那些
    // 两个数都要报（有门槛的类显示成 gated/cnt）。只报 cnt 会出这种画面：面板写
    // 「dongba x4」、工人数着 4 > 2 觉得该判 NG、板子却是 OK（判定只数了其中 2 个
    // 大的）—— 看着就像程序坏了。报了两个数，2/4 自己就把话说清楚了。
    // 过不过门槛走 countsTowardRule，跟 isNG / draw 同一处口径，不另写条件。
    std::vector<int> cnt(_classes.size(), 0);
    std::vector<int> gated(_classes.size(), 0);
    for (const auto& d : defects) {
        if (d.cls_id < 0 || d.cls_id >= (int)_classes.size()) continue;
        cnt[d.cls_id]++;
        if (countsTowardRule(d)) gated[d.cls_id]++;
    }

    // 面板行 + 每行文字颜色(跟随对应类别框的颜色)。
    // 注意: cv::putText 只支持 ASCII, 不能写中文(显示乱码), 全用英文
    std::vector<std::string> lines;
    std::vector<cv::Scalar>  colors;
    int present = 0;
    for (int c : cnt) if (c > 0) present++;
    lines.push_back("Defects | " + std::to_string(present) + " classes");
    colors.push_back(cv::Scalar(0, 255, 255));                       // 标题黄色
    for (size_t i = 0; i < _classes.size(); ++i) {
        // 没检出的类不画行。必须判 cnt: 否则某个类这次图上一个都没检出时, 面板会挂一行
        // 恒为 0 的记录, 工人看了会以为模型在识别这个类。
        if (cnt[i] == 0) continue;
        std::string line = "  " + _classes[i] + " ";
        if (hasGate(_classes[i]))
            // 有门槛的类：「算数/全部」。dongba 2/4 = 画了 4 个框，其中 2 个
            // 过了 30mm（判定就按 2 个数）。全过的时候写 4/4 而不省略 —— 形状固定，
            // 工人不用去猜这次是哪种写法。
            // ⚠ 判据是 hasGate 不是 sizeGateMm：fabai/shupi 可能把尺寸门槛调成 0（不过滤）、
            //   只留那道置信度门槛，那时它照样是「会筛掉一部分」的类，两个数不相等。
            line += std::to_string(gated[i]) + "/" + std::to_string(cnt[i]);
        else
            // 一道门槛都没有的类两个数永远相等，写 x4 就够了
            line += "x" + std::to_string(cnt[i]);
        lines.push_back(line);
        colors.push_back(classColor(_classes[i]));                   // 同框色
    }

    // 面板尺寸：字放大 2 倍(0.5→1.0)加粗, 工人远看也要能看清
    const double fs = 1.0;
    const int    thickness = 2;
    int base = 0;
    int maxw = 0, line_h = 0;
    std::vector<cv::Size> sizes(lines.size());
    for (size_t i = 0; i < lines.size(); ++i) {
        sizes[i] = cv::getTextSize(lines[i], cv::FONT_HERSHEY_SIMPLEX, fs, thickness, &base);
        maxw   = std::max(maxw, sizes[i].width);
        line_h = std::max(line_h, sizes[i].height);
    }
    const int pad = 10, gap = 8;
    int panel_w = maxw + pad * 2;
    int panel_h = (int)lines.size() * (line_h + gap) + pad * 2 - gap;

    // 左上角，超界自动缩回图内
    int px = 8, py = 8;
    if (px + panel_w > frame.cols) px = std::max(0, frame.cols - panel_w - 8);
    if (py + panel_h > frame.rows) py = std::max(0, frame.rows - panel_h - 8);

    // 半透明黑底：只混合面板那一小块区域, 不整帧操作。
    // 整帧 copyTo + addWeighted 要在 2592x1944 全图上各扫一遍(memcpy ~15MB + 混合读3写1 ~60MB 带宽),
    // Jetson 上每板额外花几十毫秒(实测单板耗时被拉到 243ms), 是耗时大头 —— 只在面板 ROI 上混合, 降到亚毫秒
    cv::Rect roi(px, py, panel_w, panel_h);
    cv::Mat  panel = frame(roi).clone();                        // 只拷面板一块
    cv::Mat  black(roi.size(), frame.type(), cv::Scalar(0, 0, 0));
    cv::addWeighted(panel, 0.60, black, 0.40, 0, frame(roi));   // 直接写回原图该区域

    // 文字：每行颜色跟随类别框颜色。
    // 先描黑边、再叠彩色 —— 深色类(如 quebian 深红)在半透明底上也像加粗一样清晰
    int ty = py + pad + line_h;
    for (size_t i = 0; i < lines.size(); ++i) {
        cv::putText(frame, lines[i], cv::Point(px + pad + 1, ty + 1),
                    cv::FONT_HERSHEY_SIMPLEX, fs, cv::Scalar(0, 0, 0), thickness + 2);
        cv::putText(frame, lines[i], cv::Point(px + pad, ty),
                    cv::FONT_HERSHEY_SIMPLEX, fs, colors[i], thickness);
        ty += line_h + gap;
    }
}
