#include "postprocessor.h"
#include "config.h"
#include <algorithm>
#include <cmath>
#include <sstream>
#include <iomanip>

// 百分比显示：支持一位小数（界面 QDoubleSpinBox 可配 0.4%，lround 会吞掉小数）
static std::string pctStr(float ratio) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(1) << ratio * 100.0f;
    return ss.str();
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

bool Postprocessor::isNG(const std::vector<Defect>& defects, const cv::Size& size,
                         float len_mm, float wid_mm, std::string& reason) const {
    int   jieba_cnt   = 0;
    int   dongba_cnt  = 0;
    float dongban_sum = 0.0f;
    float quebian_sum = 0.0f;
    float total_area  = (float)(size.width * size.height);

    for (const auto& d : defects) {
        float area = d.box.width * d.box.height;
        if (d.name == "jieba") {
            jieba_cnt++;
        } else if (d.name == "dongba") {
            dongba_cnt++;
        } else if (d.name == "dongban") {
            dongban_sum += area;
        } else if (d.name == "quebian") {
            quebian_sum += area;
        }
        // 其它类默认 OK，不判 NG
    }

    std::vector<std::string> reasons;
    if (jieba_cnt > _jieba_max_count)
        reasons.push_back("结疤>" + std::to_string(_jieba_max_count));
    if (dongba_cnt > _dongba_max_count)
        reasons.push_back("洞疤>" + std::to_string(_dongba_max_count));
    if (dongban_sum / total_area > _dongban_area_ratio)
        reasons.push_back("洞坑>" + pctStr(_dongban_area_ratio) + "%");
    if (quebian_sum / total_area > _quebian_area_ratio)
        reasons.push_back("缺边>" + pctStr(_quebian_area_ratio) + "%");
    // 组合判定：jieba+dongba 数量之和、dongban+quebian 面积之和
    if (jieba_cnt + dongba_cnt > _jieba_dongba_max_count)
        reasons.push_back("结疤+洞疤>" + std::to_string(_jieba_dongba_max_count));
    if ((dongban_sum + quebian_sum) / total_area > _dongban_quebian_area_ratio)
        reasons.push_back("洞坑+缺边>" + pctStr(_dongban_quebian_area_ratio) + "%");

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
    return !reason.empty();
}

void Postprocessor::draw(cv::Mat& frame, const std::vector<Defect>& defects) {
    for (const auto& d : defects) {
        cv::Scalar c = classColor(d.name);

        cv::rectangle(frame, d.box.tl(), d.box.br(), c, 2);

        std::ostringstream ss;
        ss << d.name << " " << std::fixed << std::setprecision(2) << d.conf;
        int bl;
        auto ts = cv::getTextSize(ss.str(), cv::FONT_HERSHEY_SIMPLEX, 0.5, 1, &bl);
        cv::rectangle(frame,
            cv::Point(d.box.x, d.box.y - ts.height - 6),
            cv::Point(d.box.x + ts.width + 4, d.box.y), c, cv::FILLED);
        cv::putText(frame, ss.str(),
            cv::Point(d.box.x + 2, d.box.y - 4),
            cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(255, 255, 255), 1);
    }
}

// 左上角统计面板：类别数 + 各类框数 + dongban/quebian 面积和(px + 占图比例)
void Postprocessor::drawSummary(cv::Mat& frame, const std::vector<Defect>& defects) {
    if (frame.empty()) return;

    // 按类别统计：框数 + 面积和
    std::vector<int>  cnt (_classes.size(), 0);
    std::vector<long> area(_classes.size(), 0);
    for (const auto& d : defects) {
        if (d.cls_id < 0 || d.cls_id >= (int)_classes.size()) continue;
        cnt[d.cls_id]++;
        area[d.cls_id] += (long)d.box.width * d.box.height;
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
        if (cnt[i] == 0) continue;
        lines.push_back("  " + _classes[i] + " x" + std::to_string(cnt[i]));
        colors.push_back(classColor(_classes[i]));                   // 同框色
    }
    float total = (float)(frame.cols * frame.rows);
    for (size_t i = 0; i < _classes.size(); ++i) {
        if (_classes[i] == "dongban" || _classes[i] == "quebian") {
            std::ostringstream ss;
            ss << _classes[i] << " area sum " << area[i] << "px ("
               << pctStr((float)area[i] / total) << "%)";
            lines.push_back(ss.str());
            colors.push_back(classColor(_classes[i]));               // 同框色
        }
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

    // 半透明黑底(轻压暗, 板子能透出来)
    cv::Mat overlay;
    frame.copyTo(overlay);
    cv::rectangle(overlay, cv::Rect(px, py, panel_w, panel_h), cv::Scalar(0, 0, 0), cv::FILLED);
    cv::addWeighted(overlay, 0.40, frame, 0.60, 0, frame);

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
