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

    if (d.name == "crack" || d.name == "edge_damage" || d.name == "rot")
        return "ng";
    if (d.name == "hole")
        return area > Config::HOLE_MAX_AREA ? "ng" : "warn";
    if (d.name == "knot") {
        if (ratio > Config::KNOT_NG_RATIO)   return "ng";
        if (ratio > Config::KNOT_WARN_RATIO) return "warn";
        return "ok";
    }
    if (d.name == "scratch") {
        float L = std::max(d.box.width, d.box.height);
        float S = std::min(d.box.width, d.box.height);
        if (L / (S + 1e-6f) > Config::SCRATCH_ASPECT && L > Config::SCRATCH_NG_LEN)
            return "ng";
        if (L > Config::SCRATCH_WARN_LEN) return "warn";
        return "ok";
    }
    if (d.name == "stain")
        return ratio > Config::STAIN_NG_RATIO ? "ng" : "warn";

    return "ok";
}

void Postprocessor::draw(cv::Mat& frame, const std::vector<Defect>& defects) {
    for (const auto& d : defects) {
        cv::Scalar c(0, 255, 0);
        if (d.name == "knot")        c = cv::Scalar(0, 255, 255);
        else if (d.name == "crack")  c = cv::Scalar(0, 0, 255);
        else if (d.name == "hole")   c = cv::Scalar(255, 0, 0);
        else if (d.name == "stain")  c = cv::Scalar(255, 0, 255);
        else if (d.name == "scratch")c = cv::Scalar(0, 165, 255);
        else if (d.name == "edge_damage") c = cv::Scalar(0, 0, 128);
        else if (d.name == "rot")    c = cv::Scalar(128, 0, 128);

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
