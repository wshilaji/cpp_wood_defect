#include "postprocessor.h"
#include "config.h"
#include <iostream>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <sys/stat.h>

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

bool Postprocessor::isNG(const std::vector<Defect>& defects, const cv::Size& size, std::string& reason) const {
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
    if (jieba_cnt > Config::JIEBA_MAX_COUNT)
        reasons.push_back("jieba>" + std::to_string(Config::JIEBA_MAX_COUNT));
    if (dongba_cnt > Config::DONGBA_MAX_COUNT)
        reasons.push_back("dongba>" + std::to_string(Config::DONGBA_MAX_COUNT));
    if (dongban_sum / total_area > Config::DONGBAN_AREA_RATIO)
        reasons.push_back("dongban>" + std::to_string(std::lround(Config::DONGBAN_AREA_RATIO * 100.0)) + "%");
    if (quebian_sum / total_area > Config::QUEBIAN_AREA_RATIO)
        reasons.push_back("quebian>" + std::to_string(std::lround(Config::QUEBIAN_AREA_RATIO * 100.0)) + "%");

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

std::string Postprocessor::save(const cv::Mat& frame,
                                 const std::vector<Defect>& defects,
                                 const std::string& dir) {
    if (defects.empty()) return "";

    mkdir(dir.c_str(), 0755);
    std::string path = dir + "/NG_" + _ts() + ".jpg";
    cv::imwrite(path, frame);
    return path;
}

std::string Postprocessor::_ts() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    std::ostringstream ss;
    ss << std::put_time(std::localtime(&t), "%Y%m%d_%H%M%S_")
       << std::setfill('0') << std::setw(3) << ms.count();
    return ss.str();
}
