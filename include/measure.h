#pragma once
#include <opencv2/opencv.hpp>
#include <cmath>
#include <iostream>
#include <vector>

/**
 * 木板长宽测量模块 — header-only
 *
 * 流程: 图像分割 → minAreaRect → pinhole 投影 → 世界坐标 mm
 */

struct BoardMeasure {
    double long_mm   = 0;   // 实际长边 mm
    double short_mm  = 0;   // 实际短边 mm
    double area_px   = 0;   // 轮廓像素面积
    cv::RotatedRect  rrect; // 最小外接旋转矩形
    std::vector<cv::Point> contour; // 木板轮廓
    bool valid       = false;
};

/**
 * OTSU 二值化 + 形态学 → 提取最大轮廓（木板主体）
 */
inline std::vector<cv::Point> segmentBoard(const cv::Mat& bgr) {
    cv::Mat gray;
    cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);

    cv::Mat blurred;
    cv::GaussianBlur(gray, blurred, cv::Size(5, 5), 0);

    cv::Mat binary;
    cv::threshold(blurred, binary, 0, 255, cv::THRESH_BINARY | cv::THRESH_OTSU);

    cv::Mat kernel = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(7, 7));
    cv::morphologyEx(binary, binary, cv::MORPH_CLOSE, kernel);
    cv::morphologyEx(binary, binary, cv::MORPH_OPEN, kernel);

    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(binary, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

    if (contours.empty()) return {};

    auto largest = std::max_element(contours.begin(), contours.end(),
        [](const std::vector<cv::Point>& a, const std::vector<cv::Point>& b) {
            return cv::contourArea(a) < cv::contourArea(b);
        });
    return *largest;
}

/**
 * pinhole 投影: 像素 (u,v) → 世界坐标 (X,Y,Z)
 */
inline cv::Point3d pixelToWorld(const cv::Point2d& px, const cv::Mat& K, double Z) {
    double fx = K.at<double>(0, 0);
    double fy = K.at<double>(1, 1);
    double cx = K.at<double>(0, 2);
    double cy = K.at<double>(1, 2);
    double X = (px.x - cx) * Z / fx;
    double Y = (px.y - cy) * Z / fy;
    return {X, Y, Z};
}

/**
 * 测量木板长宽（mm）
 * @param bgr  输入彩色图
 * @param K    相机内参 3×3
 * @param Z    物距 mm
 */
inline BoardMeasure measureBoard(const cv::Mat& bgr, const cv::Mat& K, double Z) {
    BoardMeasure m;

    auto contour = segmentBoard(bgr);
    if (contour.empty()) return m;

    m.contour = contour;
    m.area_px = cv::contourArea(contour);

    cv::RotatedRect rrect = cv::minAreaRect(contour);
    m.rrect = rrect;

    // 获取 4 个角点
    cv::Point2f corners[4];
    rrect.points(corners);

    // 角点转世界坐标
    std::vector<cv::Point3d> world(4);
    for (int i = 0; i < 4; ++i)
        world[i] = pixelToWorld(cv::Point2d(corners[i].x, corners[i].y), K, Z);

    auto dist = [](const cv::Point3d& a, const cv::Point3d& b) {
        double dx = a.x - b.x, dy = a.y - b.y;
        return std::sqrt(dx * dx + dy * dy);
    };

    double e0 = dist(world[0], world[1]);
    double e1 = dist(world[1], world[2]);
    double e2 = dist(world[2], world[3]);
    double e3 = dist(world[3], world[0]);

    double side_a = (e0 + e2) / 2.0;
    double side_b = (e1 + e3) / 2.0;

    m.long_mm  = std::max(side_a, side_b);
    m.short_mm = std::min(side_a, side_b);
    m.valid    = true;
    return m;
}

/**
 * 创建内参矩阵 K
 */
inline cv::Mat makeK(double fx, double fy, double cx, double cy) {
    return (cv::Mat_<double>(3, 3) << fx, 0.0, cx, 0.0, fy, cy, 0.0, 0.0, 1.0);
}
