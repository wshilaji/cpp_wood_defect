#pragma once

#include <opencv2/opencv.hpp>
#include <vector>
#include <array>

/**
 * 图像预处理模块
 *
 * 流水线: 缩放 → CLAHE增强 → 归一化 → 转CHW → 添加batch维度
 * 输出可直接送入 TensorRT 引擎
 */
class Preprocessor {
public:
    /**
     * @param target_w     模型输入宽度
     * @param target_h     模型输入高度
     * @param enable_clahe 是否启用 CLAHE 对比度增强
     * @param clip_limit   CLAHE 裁剪限制
     * @param tile_size    CLAHE 网格大小
     */
    Preprocessor(int target_w = 640, int target_h = 640,
                 bool enable_clahe = true,
                 float clip_limit = 2.0f, int tile_size = 8);

    /**
     * 执行预处理
     * @param frame    输入帧 (BGR, HWC, uint8)
     * @param out_tensor 输出张量 (float*, 已分配内存, 大小 = 1*C*H*W)
     * @param out_display 输出处理后的可视图像 (用于画框展示)
     */
    void process(const cv::Mat& frame, float* out_tensor, cv::Mat& out_display);

    /** 仅获取归一化后的显示用图（不分配tensor内存） */
    cv::Mat processForDisplay(const cv::Mat& frame);

    int targetWidth()  const { return _target_w; }
    int targetHeight() const { return _target_h; }

private:
    int     _target_w;
    int     _target_h;
    int     _channels;
    bool    _enable_clahe;
    float   _clip_limit;
    int     _tile_size;

    cv::Ptr<cv::CLAHE> _clahe;
};
