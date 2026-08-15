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
        cv::Scalar c(0, 255, 0);  // 默认绿色
        if (d.name == "dongba")          c = cv::Scalar(0, 120, 255);   // 深橙
        else if (d.name == "dongban")    c = cv::Scalar(160, 90, 0);    // 深青
        else if (d.name == "jieba")      c = cv::Scalar(255, 255, 0);   // 青
        else if (d.name == "shupi")      c = cv::Scalar(128, 128, 128); // 灰
        else if (d.name == "shuwen")     c = cv::Scalar(0, 165, 255);   // 橙
        else if (d.name == "heiba")      c = cv::Scalar(0, 0, 255);     // 红
        else if (d.name == "piwenba")    c = cv::Scalar(255, 0, 255);   // 品红
        else if (d.name == "quebian")    c = cv::Scalar(0, 0, 128);     // 深红
        else if (d.name == "baowen")     c = cv::Scalar(255, 165, 0);   // 蓝? 实际是BGR
        else if (d.name == "liefeng")    c = cv::Scalar(0, 0, 200);     // 深红
        else if (d.name == "suibian")    c = cv::Scalar(0, 128, 128);   // 深黄
        else if (d.name == "heiban")     c = cv::Scalar(255, 0, 0);     // 蓝
        else if (d.name == "banwen")     c = cv::Scalar(255, 0, 128);   // 紫
        else if (d.name == "banwenba")   c = cv::Scalar(200, 0, 200);   // 浅紫

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
