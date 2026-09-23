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
// ⚠ MM_PER_PX 是标定换算（config.h），相机装高装低、镜头焦距不是标称值它就偏 ——
//   所以这些「绝对毫米」门槛天生带标定误差，跟以前的面积占比那种纯像素比值不是一回事。
static float boxLongSideMm(const cv::Rect& box) {
    return (float)std::max(box.width, box.height) * Config::MM_PER_PX;
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
    if (name == "suibian")      return cv::Scalar(0, 128, 128);   // 深黄
    if (name == "heiban")       return cv::Scalar(255, 0, 0);     // 蓝
    if (name == "banwen")       return cv::Scalar(255, 0, 128);   // 紫
    if (name == "banwenba")     return cv::Scalar(200, 0, 200);   // 浅紫
    return cv::Scalar(0, 255, 0);                                 // 默认绿
}

// 「这一块不算数」的统一画法：把类色整体压暗，而不是换成另一个颜色。
// 为什么不换色：灰色已经被 shupi 占了，别的颜色又各有其主，换色会让工人以为框里
// 是另一个类。压暗则一眼看出「还是这个类，只是小到不算数」。
static cv::Scalar dimColor(const cv::Scalar& c) {
    const double k = 0.45;   // 压到 45%：暗到能区分，又不至于在深色板上看不见
    return cv::Scalar(c[0] * k, c[1] * k, c[2] * k);
}

std::vector<Defect> Postprocessor::process(const trtyolo::DetectRes& res,
                                            cv::Mat& frame, const cv::Size& size) {
    std::vector<Defect> defects;

    for (int i = 0; i < res.num; ++i) {
        if (res.scores[i] < _thresh) continue;

        const auto& b = res.boxes[i];
        int x = std::max(0, (int)b.left);
        int y = std::max(0, (int)b.top);
        int w = std::min((int)(b.right - b.left), size.width - x);
        int h = std::min((int)(b.bottom - b.top), size.height - y);

        Defect d;
        d.cls_id = res.classes[i];
        d.conf   = res.scores[i];
        d.box    = cv::Rect(x, y, w, h);
        d.name   = (d.cls_id < (int)_classes.size()) ? _classes[d.cls_id] : "?";
        defects.push_back(d);
    }

    draw(frame, defects);
    drawSummary(frame, defects);   // 左上角统计面板
    return defects;
}

// ============================================================
// NG 判定 —— 全流程唯一决定 OK/NG 的地方
//
// 判据现在只有一种: 【数量】。7 个类各有一个数量上限, 超了判 NG:
//   jieba 活节 / heiba 小油疤 —— 不管大小, 全算进数量。
//   dongba 死节 / dongban 破洞 / quebian 缺边 / shupi 树皮 / fabai 发白 ——
//       数之前先过一道【尺寸门槛】: 检测框最长边换算成毫米, 短于门槛的不计数
//       (工人界面填, 0=不过滤)。这是【过滤】不是【拒绝】: 小的不会把板子判死,
//       只是不算进它那一类的计数(死节也不算进「活节+死节」那个组合数)。
//       门槛值各类各管各的(死节 30mm 是现场标准, 其余现场还没调过)。
//       判定口径只有 countsTowardRule() 一处, draw() 用的也是它(只不过把框线压暗来画)——
//       两处必须同一个口径, 否则会出现「画成不算数的颜色、却被数进去判了 NG」。
//
// 为什么全改成数量(2026-09-23 一天里改完的):
//   原来 dongban/quebian/shupi/fabai 走的是【面积和占整图比例】。那个口径的根本毛病是
//   它把「一堆小的」和「一个大的」算成同一个数 —— 30 个小缺陷的面积和可以大于 1 个大
//   缺陷, 但这两块板该不该扔正好相反。尺寸门槛 + 数量才分得开, 而且门槛是工人听得懂的
//   说法(「大于 3 公分的才算」), 面积占比则是个谁也没法目测的数。
//   注: 面积算的是检测框 w*h, 不是真实缺陷像素面积(圆形框比实际大约 1.27 倍), 这也是面积
//   口径不好用的一面。换成最长边之后没有这个膨胀 —— 最长边本来就是框上直接量的。
//
// 另有一条组合规则: 单类都没超、但两类加起来超了也要拦(活节+死节)。
// 曾经还有一条「破洞+缺边面积大于」, 破洞/缺边都改走数量之后凑不出合并口径(个数和面积
// 没法相加), 2026-09-23 现场删掉了。
//
// 其余类(shuwen/piwenba/baowen/liefeng/suibian/heiban/banwen/banwenba)
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
    if (name == "quebian") return _quebian_min_len_mm;
    if (name == "shupi")   return _shupi_min_len_mm;
    if (name == "fabai")   return _fabai_min_len_mm;
    return 0;
}

bool Postprocessor::countsTowardRule(const Defect& d) const {
    const int gate = sizeGateMm(d.name);
    if (gate <= 0) return true;                                   // 没门槛 / 门槛关掉
    return boxLongSideMm(d.box) > (float)gate;
}

bool Postprocessor::isNG(const std::vector<Defect>& defects,
                         float len_mm, float wid_mm, std::string& reason,
                         bool* size_only) const {
    int jieba_cnt   = 0;
    int dongba_cnt  = 0;      // 下面 5 个都只数过了尺寸门槛的那些，见 countsTowardRule
    int dongban_cnt = 0;
    int quebian_cnt = 0;
    int heiba_cnt   = 0;
    int shupi_cnt   = 0;
    int fabai_cnt   = 0;

    for (const auto& d : defects) {
        // 没门槛的类直接数；有门槛的类先问 countsTowardRule。门槛只管计数，
        // 框该画还画（draw 把不过门槛的框线压暗）。
        if (d.name == "jieba") {
            jieba_cnt++;
        } else if (d.name == "heiba") {
            heiba_cnt++;
        } else if (d.name == "dongba") {
            if (countsTowardRule(d)) dongba_cnt++;
        } else if (d.name == "dongban") {
            if (countsTowardRule(d)) dongban_cnt++;
        } else if (d.name == "quebian") {
            if (countsTowardRule(d)) quebian_cnt++;
        } else if (d.name == "shupi") {
            if (countsTowardRule(d)) shupi_cnt++;
        } else if (d.name == "fabai") {
            if (countsTowardRule(d)) fabai_cnt++;
        }
        // 其余类(shuwen/piwenba/baowen/liefeng/suibian/heiban/banwen/banwenba)
        // 默认 OK，不判 NG，只在左上角面板画框显示
    }

    std::vector<std::string> reasons;
    // 原因串拼法：带尺寸门槛的类必须把门槛写进去，否则工人看到「死节>2」会去数图上所有
    // 该类框（包括不算数的小块），数出来比 2 多，以为程序数错了。
    // gate_mm 传 0 就是不写门槛（jieba/heiba 没这道门）。
    auto countReason = [](const char* cn, int max_cnt, int gate_mm) {
        std::string s = std::string(cn) + ">" + std::to_string(max_cnt);
        if (gate_mm > 0) s += "(" + std::to_string(gate_mm) + "mm以上)";
        return s;
    };
    // 顺序沿用改口径之前那一版（活节/死节/小油疤/破洞/缺边/树皮/发白），
    // 别按分类重排 —— 现场是照着这一串的字面顺序在看板的。
    if (jieba_cnt > _jieba_max_count)
        reasons.push_back(countReason("活节", _jieba_max_count, 0));
    if (dongba_cnt > _dongba_max_count)
        reasons.push_back(countReason("死节", _dongba_max_count, _dongba_min_len_mm));
    if (heiba_cnt > _heiba_max_count)
        reasons.push_back(countReason("小油疤", _heiba_max_count, 0));
    if (dongban_cnt > _dongban_max_count)
        reasons.push_back(countReason("破洞", _dongban_max_count, _dongban_min_len_mm));
    if (quebian_cnt > _quebian_max_count)
        reasons.push_back(countReason("缺边", _quebian_max_count, _quebian_min_len_mm));
    if (shupi_cnt > _shupi_max_count)
        reasons.push_back(countReason("树皮", _shupi_max_count, _shupi_min_len_mm));
    if (fabai_cnt > _fabai_max_count)
        reasons.push_back(countReason("发白", _fabai_max_count, _fabai_min_len_mm));
    // 组合规则：单类都没超，但活节+死节加起来超了也要拦。
    // 数的是上面那两个（死节已过门槛），活节那半边没门槛 —— 这条读作
    // 「所有活节 + 只算够大的死节」，跟单类规则同一个口径。
    // 门槛要写进原因串，而且得点明是死节的门槛（「死节30mm以上」）：这条规则里两个类
    // 只有一个带门槛，不写出是哪一边的，工人会拿 30mm 去量活节。
    // 这条没法直接用上面的 countReason —— 那个只输出「(30mm以上)」，不带类名。
    if (jieba_cnt + dongba_cnt > _jieba_dongba_max_count) {
        std::string s = "活节+死节>" + std::to_string(_jieba_dongba_max_count);
        if (_dongba_min_len_mm > 0)
            s += "(死节" + std::to_string(_dongba_min_len_mm) + "mm以上)";
        reasons.push_back(s);
    }

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
        // 没过尺寸门槛的缺陷照样画框，但框线压暗一档 —— 一眼分出「这个不算数」。
        // 判断用的是 countsTowardRule，跟 isNG 数个数时同一个口径，颜色和结论不会打架。
        // ⚠ 标签底色【不】跟着压暗：现场要求标签保持一致（一眼看清是哪个类），
        //   所以算不算数只体现在框线上。
        const cv::Scalar cls   = classColor(d.name);
        const cv::Scalar box_c = countsTowardRule(d) ? cls : dimColor(cls);

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

    // 按类别统计框数。
    // 注意：这里数的是【画出来的框】，不过尺寸门槛的小块也数在里面 —— 面板的字面意思
    // 就是「这帧里这个类有几个框」，跟 isNG 数的那份（过了门槛的）故意不是一回事：
    // 工人是对着图数的，图上画了几个框面板就该说几个，对不上才是问题。
    std::vector<int> cnt(_classes.size(), 0);
    for (const auto& d : defects) {
        if (d.cls_id < 0 || d.cls_id >= (int)_classes.size()) continue;
        cnt[d.cls_id]++;
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
        // 没检出的类不画行。必须判 cnt: 否则「模型根本吐不出这个类」时(比如 fabai
        // 在 CLASSES 里但旧模型只有 14 类)、或者这次图上一个都没检出时, 面板会挂一行
        // 恒为 0 的记录, 工人看了会以为模型在识别这个类。
        if (cnt[i] == 0) continue;
        lines.push_back("  " + _classes[i] + " x" + std::to_string(cnt[i]));
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
