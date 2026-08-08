#include "postprocessor.h"
#include "config.h"
#include <iostream>
#include <algorithm>
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
        d.level  = severity(d, size);
        defects.push_back(d);
    }

    draw(frame, defects);
    return defects;
}

std::string Postprocessor::severity(const Defect& d, const cv::Size& size) {
    float area = d.box.width * d.box.height;
    float ratio = area / (size.width * size.height);

    // 裂缝 / 缺边 / 碎边 → 直接 NG
    if (d.name == "liefeng" || d.name == "quebian" || d.name == "suibian")
        return "ng";

    // 洞疤 / 洞斑 → 面积判定
    if (d.name == "dongba" || d.name == "dongban")
        return area > Config::HOLE_MAX_AREA ? "ng" : "warn";

    // 节疤 → 面积占比判定
    if (d.name == "jieba") {
        if (ratio > Config::KNOT_NG_RATIO)   return "ng";
        if (ratio > Config::KNOT_WARN_RATIO) return "warn";
        return "ok";
    }

    // 树纹 / 皮纹疤 / 薄纹 → 长宽比 + 长度判定
    if (d.name == "shuwen" || d.name == "piwenba" || d.name == "baowen") {
        float L = std::max(d.box.width, d.box.height);
        float S = std::min(d.box.width, d.box.height);
        if (L / (S + 1e-6f) > Config::SCRATCH_ASPECT && L > Config::SCRATCH_NG_LEN)
            return "ng";
        if (L > Config::SCRATCH_WARN_LEN) return "warn";
        return "ok";
    }

    // 黑疤 / 黑斑 / 斑纹 / 斑纹疤 → 面积占比判定
    if (d.name == "heiba" || d.name == "heiban" ||
        d.name == "banwen" || d.name == "banwenba")
        return ratio > Config::STAIN_NG_RATIO ? "ng" : "warn";

    // shupi（树皮）等其他 → 默认 OK
    return "ok";
}

void Postprocessor::draw(cv::Mat& frame, const std::vector<Defect>& defects) {
    for (const auto& d : defects) {
        cv::Scalar c(0, 255, 0);  // 默认绿色
        if (d.name == "dongba")          c = cv::Scalar(0, 255, 255);   // 黄
        else if (d.name == "dongban")    c = cv::Scalar(0, 200, 200);   // 浅黄
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
        ss << d.name << " " << std::fixed << std::setprecision(2) << d.conf
           << " [" << d.level << "]";
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
    bool ng = false, warn = false;
    for (const auto& d : defects) {
        if (d.level == "ng") ng = true;
        if (d.level == "warn") warn = true;
    }
    if (!ng && !warn) return "";

    mkdir(dir.c_str(), 0755);
    std::string path = dir + "/" + (ng ? "NG_" : "WARN_") + _ts() + ".jpg";
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
