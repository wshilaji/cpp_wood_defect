#include "postprocessor.h"
#include "config.h"

#include <iostream>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <sys/stat.h>

// ============================================================
// 构造
// ============================================================
Postprocessor::Postprocessor(float conf_threshold,
                             float nms_threshold,
                             const std::vector<std::string>& class_names)
    : _conf_threshold(conf_threshold)
    , _nms_threshold(nms_threshold)
    , _class_names(class_names)
{
}

// ============================================================
// 完整后处理流水线
// ============================================================
std::vector<DefectResult> Postprocessor::process(
    float** raw_outputs,
    int num_outputs,
    const std::vector<size_t>& output_sizes,
    cv::Mat& display_frame,
    int input_w, int input_h)
{
    // Step 1: 解码 + NMS
    auto defects = decodeAndNMS(raw_outputs, num_outputs, output_sizes,
                                display_frame.rows, display_frame.cols,
                                input_h, input_w);

    // Step 2: 判定严重程度
    cv::Size frame_size(display_frame.cols, display_frame.rows);
    for (auto& d : defects) {
        Severity sev = checkSeverity(d, frame_size);
        switch (sev) {
            case Severity::OK:  d.severity = "ok";   break;
            case Severity::WARN: d.severity = "warn"; break;
            case Severity::NG:   d.severity = "ng";   break;
        }
    }

    // Step 3: 画框
    drawResults(display_frame, defects);

    return defects;
}

// ============================================================
// 模型输出解码 + NMS
//
// 假定输出格式为 YOLO 风格:
//   output[0]: float[N][5+num_classes]
//   每行: [cx, cy, w, h, obj_conf, class_0_conf, class_1_conf, ...]
// ============================================================
std::vector<DefectResult> Postprocessor::decodeAndNMS(
    float** raw_outputs,
    int num_outputs,
    const std::vector<size_t>& output_sizes,
    int frame_h, int frame_w,
    int input_h, int input_w)
{
    std::vector<Detection> detections;

    // 遍历所有输出层（多尺度输出模型）
    for (int o = 0; o < num_outputs; ++o) {
        float* output = raw_outputs[o];
        size_t size = output_sizes[o];

        // 推断该层的网格尺寸:
        // 输出总元素 = N * (5 + num_classes)
        // 每个检测点 = 5 + num_classes 个 float
        int num_classes = static_cast<int>(_class_names.size()) - 1; // 减 background
        int stride_size = 5 + num_classes;
        int num_detections = static_cast<int>(size / sizeof(float)) / stride_size;

        for (int i = 0; i < num_detections; ++i) {
            float* det = output + i * stride_size;

            float cx     = det[0];
            float cy     = det[1];
            float w      = det[2];
            float h      = det[3];
            float obj_conf = det[4];

            // 找最大类别分数
            float max_cls_score = 0.0f;
            int   max_cls_id    = 0;
            for (int c = 0; c < num_classes; ++c) {
                float score = det[5 + c];
                if (score > max_cls_score) {
                    max_cls_score = score;
                    max_cls_id = c + 1; // 跳过 background，class_id 从 1 开始
                }
            }

            float confidence = obj_conf * max_cls_score;
            if (confidence < _conf_threshold) continue;

            // 坐标从归一化转为像素值（相对于输入尺寸）
            float x1 = (cx - w / 2.0f) * input_w;
            float y1 = (cy - h / 2.0f) * input_h;
            float x2 = (cx + w / 2.0f) * input_w;
            float y2 = (cy + h / 2.0f) * input_h;

            // 裁剪到有效范围
            x1 = std::max(0.0f, std::min(x1, static_cast<float>(input_w)));
            y1 = std::max(0.0f, std::min(y1, static_cast<float>(input_h)));
            x2 = std::max(0.0f, std::min(x2, static_cast<float>(input_w)));
            y2 = std::max(0.0f, std::min(y2, static_cast<float>(input_h)));

            cv::Rect box;
            box.x = static_cast<int>(x1);
            box.y = static_cast<int>(y1);
            box.width  = static_cast<int>(x2 - x1);
            box.height = static_cast<int>(y2 - y1);

            if (box.width <= 0 || box.height <= 0) continue;

            Detection d;
            d.box = box;
            d.confidence = confidence;
            d.class_id = max_cls_id;
            detections.push_back(d);
        }
    }

    // ---- NMS ----
    auto nms_results = applyNMS(detections);

    // ---- 缩放到显示帧尺寸 ----
    float scale_x = static_cast<float>(frame_w) / input_w;
    float scale_y = static_cast<float>(frame_h) / input_h;

    std::vector<DefectResult> results;
    for (const auto& d : nms_results) {
        DefectResult r;
        r.class_id   = d.class_id;
        r.confidence = d.confidence;
        r.box = cv::Rect(
            static_cast<int>(d.box.x * scale_x),
            static_cast<int>(d.box.y * scale_y),
            static_cast<int>(d.box.width  * scale_x),
            static_cast<int>(d.box.height * scale_y)
        );
        r.class_name = (d.class_id < static_cast<int>(_class_names.size()))
                           ? _class_names[d.class_id]
                           : "unknown";
        results.push_back(r);
    }

    return results;
}

// ============================================================
// NMS
// ============================================================
std::vector<Postprocessor::Detection>
Postprocessor::applyNMS(const std::vector<Detection>& detections)
{
    if (detections.empty()) return {};

    // 按置信度降序排列
    auto sorted = detections;
    std::sort(sorted.begin(), sorted.end());

    std::vector<Detection> keep;
    std::vector<bool> suppressed(sorted.size(), false);

    for (size_t i = 0; i < sorted.size(); ++i) {
        if (suppressed[i]) continue;

        keep.push_back(sorted[i]);

        // 抑制与当前框 IoU 过高的后续框（同类才抑制）
        for (size_t j = i + 1; j < sorted.size(); ++j) {
            if (suppressed[j]) continue;
            // 不同类别不互相抑制
            if (sorted[i].class_id != sorted[j].class_id) continue;

            float iou = computeIoU(sorted[i].box, sorted[j].box);
            if (iou > _nms_threshold) {
                suppressed[j] = true;
            }
        }
    }

    return keep;
}

// ============================================================
// IoU 计算
// ============================================================
float Postprocessor::computeIoU(const cv::Rect& a, const cv::Rect& b) {
    int inter_x1 = std::max(a.x, b.x);
    int inter_y1 = std::max(a.y, b.y);
    int inter_x2 = std::min(a.x + a.width,  b.x + b.width);
    int inter_y2 = std::min(a.y + a.height, b.y + b.height);

    int inter_w = std::max(0, inter_x2 - inter_x1);
    int inter_h = std::max(0, inter_y2 - inter_y1);
    float inter_area = static_cast<float>(inter_w * inter_h);

    float area_a = static_cast<float>(a.width * a.height);
    float area_b = static_cast<float>(b.width * b.height);
    float union_area = area_a + area_b - inter_area;

    return (union_area > 0.0f) ? (inter_area / union_area) : 0.0f;
}

// ============================================================
// 规则引擎: 瑕疵严重程度判定
// ============================================================
Severity Postprocessor::checkSeverity(const DefectResult& defect,
                                       const cv::Size& frame_size)
{
    float area = static_cast<float>(defect.box.width * defect.box.height);
    float frame_area = static_cast<float>(frame_size.width * frame_size.height);
    float area_ratio = area / frame_area;

    const auto& cls = defect.class_name;

    // ---- 裂纹: 任意大小都 NG ----
    if (cls == "crack") {
        return Severity::NG;
    }

    // ---- 孔洞: 面积过大 → NG ----
    if (cls == "hole") {
        if (area > Config::Rule::HOLE_MAX_AREA) {
            return Severity::NG;
        }
        return Severity::WARN;
    }

    // ---- 节疤: 分级判定 ----
    if (cls == "knot") {
        if (area_ratio > Config::Rule::KNOT_NG_RATIO)   return Severity::NG;
        if (area_ratio > Config::Rule::KNOT_WARN_RATIO) return Severity::WARN;
        return Severity::OK;
    }

    // ---- 划痕: 长宽比 + 长度判定 ----
    if (cls == "scratch") {
        float max_len = static_cast<float>(std::max(defect.box.width,
                                                     defect.box.height));
        float min_len = static_cast<float>(std::min(defect.box.width,
                                                     defect.box.height));
        float aspect = max_len / (min_len + 1e-6f);

        if (aspect > Config::Rule::SCRATCH_ASPECT &&
            max_len > Config::Rule::SCRATCH_NG_LENGTH) {
            return Severity::NG;
        }
        if (max_len > Config::Rule::SCRATCH_WARN_LENGTH) {
            return Severity::WARN;
        }
        return Severity::OK;
    }

    // ---- 边角破损 → NG ----
    if (cls == "edge_damage") {
        return Severity::NG;
    }

    // ---- 腐朽 → NG ----
    if (cls == "rot") {
        return Severity::NG;
    }

    // ---- 污渍: 大面积 → NG ----
    if (cls == "stain") {
        if (area_ratio > Config::Rule::STAIN_NG_RATIO) {
            return Severity::NG;
        }
        return Severity::WARN;
    }

    return Severity::OK;
}

// ============================================================
// 可视化: 画框
// ============================================================
void Postprocessor::drawResults(cv::Mat& frame,
                                 const std::vector<DefectResult>& results)
{
    // 瑕疵颜色表
    static const std::map<std::string, cv::Scalar> COLORS = {
        {"knot",         cv::Scalar(0, 255, 255)},   // 黄
        {"crack",        cv::Scalar(0, 0, 255)},     // 红
        {"hole",         cv::Scalar(255, 0, 0)},     // 蓝
        {"stain",        cv::Scalar(255, 0, 255)},   // 紫
        {"scratch",      cv::Scalar(0, 165, 255)},   // 橙
        {"edge_damage",  cv::Scalar(0, 0, 128)},     // 深红
        {"rot",          cv::Scalar(128, 0, 128)},   // 深紫
    };

    for (const auto& defect : results) {
        if (defect.class_id == 0) continue; // skip background

        cv::Scalar color(0, 255, 0); // 默认绿色
        auto it = COLORS.find(defect.class_name);
        if (it != COLORS.end()) {
            color = it->second;
        }

        // 画矩形框
        cv::rectangle(frame,
                      cv::Point(defect.box.x, defect.box.y),
                      cv::Point(defect.box.x + defect.box.width,
                                defect.box.y + defect.box.height),
                      color, 2);

        // 标签文字
        std::ostringstream label;
        label << defect.class_name << ": "
              << std::fixed << std::setprecision(2) << defect.confidence
              << " [" << defect.severity << "]";

        int baseline = 0;
        cv::Size text_size = cv::getTextSize(label.str(),
                                              cv::FONT_HERSHEY_SIMPLEX,
                                              0.5, 1, &baseline);

        // 标签背景
        cv::rectangle(frame,
                      cv::Point(defect.box.x, defect.box.y - text_size.height - 8),
                      cv::Point(defect.box.x + text_size.width + 4, defect.box.y),
                      color, cv::FILLED);

        // 标签文字（黑底白字或白底黑字）
        cv::putText(frame, label.str(),
                    cv::Point(defect.box.x + 2, defect.box.y - 4),
                    cv::FONT_HERSHEY_SIMPLEX, 0.5,
                    cv::Scalar(255, 255, 255), 1);
    }
}

// ============================================================
// 保存瑕疵图片
// ============================================================
std::string Postprocessor::saveDefectImage(
    const cv::Mat& frame,
    const std::vector<DefectResult>& results,
    const std::string& out_dir)
{
    // 检查是否有 NG
    bool has_ng = false, has_warn = false;
    for (const auto& r : results) {
        if (r.severity == "ng")   { has_ng   = true; break; }
        if (r.severity == "warn") { has_warn = true; }
    }
    if (!has_ng && !has_warn) return "";

    // 创建输出目录
    if (mkdir(out_dir.c_str(), 0755) != 0 && errno != EEXIST) {
        std::cerr << "[Postprocessor] WARNING: 无法创建输出目录 " << out_dir << std::endl;
        return "";
    }

    // 生成文件名
    std::string prefix = has_ng ? "NG" : "WARN";
    std::string filename = out_dir + "/" + prefix + "_" + timestamp() + ".jpg";

    if (cv::imwrite(filename, frame)) {
        std::cout << "[Postprocessor] 瑕疵图片已保存: " << filename << std::endl;
        return filename;
    }

    std::cerr << "[Postprocessor] ERROR: 写入图片失败 " << filename << std::endl;
    return "";
}

std::string Postprocessor::timestamp() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()) % 1000;

    std::ostringstream oss;
    oss << std::put_time(std::localtime(&t), "%Y%m%d_%H%M%S")
        << "_" << std::setfill('0') << std::setw(3) << ms.count();
    return oss.str();
}
