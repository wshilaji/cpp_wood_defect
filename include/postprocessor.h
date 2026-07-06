#pragma once

#include <opencv2/opencv.hpp>
#include <string>
#include <vector>
#include <array>
#include <functional>

/**
 * 后处理与瑕疵判定模块
 *
 * 负责:
 *  1. 解码模型输出 (boxes, scores, class_ids)
 *  2. NMS 去重
 *  3. 严重程度判定 (OK / WARN / NG)
 *  4. 可视化画框
 *  5. 保存瑕疵图片
 */

// 缺陷检测结果
struct DefectResult {
    int     class_id;
    float   confidence;
    cv::Rect box;                   // x, y, w, h
    std::string class_name;
    std::string severity;           // "ok" / "warn" / "ng"
    std::string reason;
};

enum class Severity {
    OK = 0,
    WARN = 1,
    NG = 2,
};

class Postprocessor {
public:
    Postprocessor(float conf_threshold = 0.5f,
                  float nms_threshold = 0.4f,
                  const std::vector<std::string>& class_names = {"background", "defect"});

    /**
     * 完整的后处理流水线
     * @param raw_outputs     模型原始输出
     * @param num_outputs     输出 tensor 数量
     * @param output_sizes    每个输出 tensor 的字节数
     * @param display_frame   显示用图像（被标注）
     * @param input_w         模型输入宽度
     * @param input_h         模型输入高度
     * @return 检测到的所有缺陷
     */
    std::vector<DefectResult> process(float** raw_outputs,
                                       int num_outputs,
                                       const std::vector<size_t>& output_sizes,
                                       cv::Mat& display_frame,
                                       int input_w, int input_h);

    /**
     * 单独调用: 解析 + NMS（不解码模型输出时可复用）
     */
    std::vector<DefectResult> decodeAndNMS(float** raw_outputs,
                                            int num_outputs,
                                            const std::vector<size_t>& output_sizes,
                                            int frame_h, int frame_w,
                                            int input_h, int input_w);

    /**
     * 判定瑕疵严重程度（规则引擎）
     */
    Severity checkSeverity(const DefectResult& defect,
                           const cv::Size& frame_size);

    /**
     * 画检测框和标签
     */
    void drawResults(cv::Mat& frame,
                     const std::vector<DefectResult>& results);

    /**
     * 保存瑕疵图片
     * @param frame    标注后的图像
     * @param results  检测结果
     * @param out_dir  输出目录
     * @return 保存的文件路径，为空表示未保存
     */
    std::string saveDefectImage(const cv::Mat& frame,
                                const std::vector<DefectResult>& results,
                                const std::string& out_dir = "./output/");

private:
    float _conf_threshold;
    float _nms_threshold;
    std::vector<std::string> _class_names;

    // 用于 NMS 的辅助结构
    struct Detection {
        cv::Rect box;
        float    confidence;
        int      class_id;

        bool operator<(const Detection& other) const {
            return confidence > other.confidence; // 降序排列
        }
    };

    /** 计算两个矩形的 IoU */
    static float computeIoU(const cv::Rect& a, const cv::Rect& b);

    /** 自定义 NMS 实现 */
    std::vector<Detection> applyNMS(const std::vector<Detection>& detections);

    /** 获取当前时间戳字符串 */
    static std::string timestamp();
};
