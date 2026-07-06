#include "preprocessor.h"
#include <iostream>

Preprocessor::Preprocessor(int target_w, int target_h,
                           bool enable_clahe,
                           float clip_limit, int tile_size)
    : _target_w(target_w)
    , _target_h(target_h)
    , _channels(3)
    , _enable_clahe(enable_clahe)
    , _clip_limit(clip_limit)
    , _tile_size(tile_size)
{
    if (_enable_clahe) {
        _clahe = cv::createCLAHE(_clip_limit,
                                  cv::Size(_tile_size, _tile_size));
    }
}

void Preprocessor::process(const cv::Mat& frame,
                            float* out_tensor,
                            cv::Mat& out_display)
{
    // ----------------------------------------------------------
    // Step 1: 降采样到模型输入分辨率
    // ----------------------------------------------------------
    cv::Mat resized;
    cv::resize(frame, resized, cv::Size(_target_w, _target_h),
               0, 0, cv::INTER_LINEAR);

    // ----------------------------------------------------------
    // Step 2: CLAHE 增强（在 LAB 色彩空间的 L 通道上）
    // ----------------------------------------------------------
    cv::Mat enhanced;
    if (_enable_clahe && _clahe) {
        cv::Mat lab;
        cv::cvtColor(resized, lab, cv::COLOR_BGR2Lab);

        // 分离通道
        std::vector<cv::Mat> lab_channels(3);
        cv::split(lab, lab_channels);

        // 对 L 通道做 CLAHE
        _clahe->apply(lab_channels[0], lab_channels[0]);

        // 合并
        cv::merge(lab_channels, lab);
        cv::cvtColor(lab, enhanced, cv::COLOR_Lab2BGR);
    } else {
        enhanced = resized;
    }

    // 保存增强后的图像给外部用于显示
    out_display = enhanced.clone();

    // ----------------------------------------------------------
    // Step 3: BGR → RGB, 归一化到 [0, 1], HWC → CHW
    // ----------------------------------------------------------
    cv::Mat rgb;
    cv::cvtColor(enhanced, rgb, cv::COLOR_BGR2RGB);

    // 转换为 float 并归一化
    rgb.convertTo(rgb, CV_32FC3, 1.0 / 255.0);

    // HWC → CHW: 直接逐通道写入输出 buffer
    const int plane_size = _target_w * _target_h;
    const float* data = reinterpret_cast<float*>(rgb.data);

    for (int h = 0; h < _target_h; ++h) {
        for (int w = 0; w < _target_w; ++w) {
            int hwc_idx = (h * _target_w + w) * 3; // HWC index
            for (int c = 0; c < 3; ++c) {
                out_tensor[c * plane_size + h * _target_w + w] = data[hwc_idx + c];
            }
        }
    }
}

cv::Mat Preprocessor::processForDisplay(const cv::Mat& frame) {
    cv::Mat resized;
    cv::resize(frame, resized, cv::Size(_target_w, _target_h),
               0, 0, cv::INTER_LINEAR);

    if (_enable_clahe && _clahe) {
        cv::Mat lab;
        cv::cvtColor(resized, lab, cv::COLOR_BGR2Lab);
        std::vector<cv::Mat> lab_channels(3);
        cv::split(lab, lab_channels);
        _clahe->apply(lab_channels[0], lab_channels[0]);
        cv::merge(lab_channels, lab);
        cv::cvtColor(lab, resized, cv::COLOR_Lab2BGR);
    }

    return resized;
}
