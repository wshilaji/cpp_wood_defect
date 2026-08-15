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

    /** 工人可调：jieba 数量超过此值判 NG（运行时可改，界面输入框控制） */
    void setJiebaMaxCount(int n) { _jieba_max_count = n; }
    int  jiebaMaxCount() const   { return _jieba_max_count; }

    /** 工人可调：dongba 数量超过此值判 NG */
    void setDongbaMaxCount(int n) { _dongba_max_count = n; }
    int  dongbaMaxCount() const   { return _dongba_max_count; }

    /** 工人可调：dongban 面积和占整图比例超过此值判 NG */
    void  setDongbanAreaRatio(float r) { _dongban_area_ratio = r; }
    float dongbanAreaRatio() const     { return _dongban_area_ratio; }

    /** 工人可调：quebian 面积和占整图比例超过此值判 NG */
    void  setQuebianAreaRatio(float r) { _quebian_area_ratio = r; }
    float quebianAreaRatio() const     { return _quebian_area_ratio; }

    /** 工人可调：jieba+dongba 数量之和超过此值判 NG */
    void setJiebaDongbaMaxCount(int n) { _jieba_dongba_max_count = n; }
    int  jiebaDongbaMaxCount() const   { return _jieba_dongba_max_count; }

    /** 工人可调：dongban+quebian 面积之和占比超过此值判 NG */
    void  setDongbanQuebianAreaRatio(float r) { _dongban_quebian_area_ratio = r; }
    float dongbanQuebianAreaRatio() const     { return _dongban_quebian_area_ratio; }

private:
    float _thresh;
    std::vector<std::string> _classes;
    int   _jieba_max_count            = Config::JIEBA_MAX_COUNT;            // 运行时阈值，默认 8
    int   _dongba_max_count           = Config::DONGBA_MAX_COUNT;           // 默认 8
    float _dongban_area_ratio         = Config::DONGBAN_AREA_RATIO;         // 默认 1%
    float _quebian_area_ratio         = Config::QUEBIAN_AREA_RATIO;         // 默认 1%
    int   _jieba_dongba_max_count     = Config::JIEBA_DONGBA_MAX_COUNT;     // 默认 12
    float _dongban_quebian_area_ratio = Config::DONGBAN_QUEBIAN_AREA_RATIO; // 默认 1.5%
};
