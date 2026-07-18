#pragma once
#include <opencv2/opencv.hpp>
#include <string>
#include <memory>
#include "trtyolo.hpp"

class InferEngine {
public:
    bool load(const std::string& engine_path) {
        trtyolo::InferOption opt;
        opt.enableSwapRB();  // BGR→RGB
        _model = std::make_unique<trtyolo::DetectModel>(engine_path, opt);
        return true;
    }

    trtyolo::DetectRes detect(const cv::Mat& frame) {
        trtyolo::Image img(const_cast<uchar*>(frame.data), frame.cols, frame.rows);
        return _model->predict(img);
    }

private:
    std::unique_ptr<trtyolo::DetectModel> _model;
};
