#pragma once
#include <opencv2/opencv.hpp>
#include <string>
#include <vector>
#include "trtyolo.hpp"

struct Defect {
    int cls_id;
    float conf;
    cv::Rect box;
    std::string name;
};

class Postprocessor {
public:
    Postprocessor(float thresh, const std::vector<std::string>& classes)
        : _thresh(thresh), _classes(classes) {}

    std::vector<Defect> process(const trtyolo::DetectRes& res,
                                cv::Mat& frame, const cv::Size& size);

    /** 整体 NG 判定：jieba/dongba 按数量，dongban/quebian 按面积占比(占整图)，其它默认 OK */
    bool isNG(const std::vector<Defect>& defects, const cv::Size& size) const;
    void draw(cv::Mat& frame, const std::vector<Defect>& defects);
    std::string save(const cv::Mat& frame, const std::vector<Defect>& defects,
                     const std::string& dir);

private:
    float _thresh;
    std::vector<std::string> _classes;
    static std::string _ts();
};
