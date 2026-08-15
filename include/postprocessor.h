#pragma once
#include <opencv2/opencv.hpp>
#include <string>
#include <vector>
#include "trtyolo.hpp"
#include "config.h"

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

    /** 整体 NG 判定：jieba/dongba 按数量，dongban/quebian 按面积占比(占整图)，其它默认 OK；reason 输出 NG 原因 */
    bool isNG(const std::vector<Defect>& defects, const cv::Size& size, std::string& reason) const;
    void draw(cv::Mat& frame, const std::vector<Defect>& defects);
    std::string save(const cv::Mat& frame, const std::vector<Defect>& defects,
                     const std::string& dir);

    /** 工人可调：jieba 数量超过此值判 NG（运行时可改，界面输入框控制） */
    void setJiebaMaxCount(int n) { _jieba_max_count = n; }
    int  jiebaMaxCount() const   { return _jieba_max_count; }

private:
    float _thresh;
    std::vector<std::string> _classes;
    int _jieba_max_count = Config::JIEBA_MAX_COUNT;   // 运行时阈值，默认 8
    static std::string _ts();
};
