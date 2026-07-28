/**
 * demo_widthheight.cpp — 木板长宽测量 Demo
 *
 * 流程:
 *   1. 加载相机标定参数（内参矩阵 K + 畸变系数 D）
 *   2. 读取木板图像 + 畸变矫正（undistort）
 *   3. 图像分割 → 提取木板主体轮廓（OTSU 二值化 + 最大轮廓）
 *   4. cv::minAreaRect 拟合最小外接旋转矩形
 *   5. 利用标定参数 + 物距（Z）计算实际长宽（mm）
 *
 * 相机标定方法:
 *   打印棋盘格（如 9×6 内角点），从不同角度拍 20+ 张，
 *   用 cv::findChessboardCorners + cv::calibrateCamera 求解 K 和 D。
 *   OpenCV 官方教程: https://docs.opencv.org/4.x/dc/dbb/tutorial_py_calibration.html
 *
 * 运行: ./demo_widthheight
 */

#include <opencv2/opencv.hpp>
#include <iostream>
#include <cmath>
#include <iomanip>

// ============================================================
// 1. 相机标定参数（通过棋盘格标定获得，下面为示意占位值）
//    标定后替换为实际值即可
// ============================================================

// ---- 内参矩阵 K 3×3 ----
// 若已知相机传感器参数，也可按如下公式估算（仅作初始值）:
//   fx = fy = (sensor_width_mm / image_width_px) * 焦距_mm 的反比关系
//   例如: 1/2.5" 传感器 (5.76×4.29mm), 焦距 6mm, 图像 1920×1080
//         fx ≈ 6 / (5.76 / 1920) = 2000
//         fy ≈ 6 / (4.29 / 1080) = 1510
//   cx = image_width  / 2,  cy = image_height / 2
//
// 占位值（用图像分辨率的一半作为光心）:
static double FX = 2000.0;   // 替换为标定得到的 fx
static double FY = 2000.0;   // 替换为标定得到的 fy
static double CX = 1224.0;   // 替换为标定得到的 cx  (image_width/2)
static double CY = 1024.0;   // 替换为标定得到的 cy  (image_height/2)

static cv::Mat cameraMatrix() {
    // clang-format off
    return (cv::Mat_<double>(3, 3) <<
        FX,  0.0, CX,
        0.0, FY,  CY,
        0.0, 0.0, 1.0);
    // clang-format on
}

// ---- 畸变系数 (k1, k2, p1, p2, k3) ----
// 占位值（零畸变——未标定时先用这个，至少能看到结果）:
static cv::Mat distCoeffs() {
    return (cv::Mat_<double>(1, 5) << 0.0, 0.0, 0.0, 0.0, 0.0);
}

// ---- 物距 Z（相机镜头到木板平面的距离，单位 mm） ----
// 实际部署时测量相机安装高度
static double DISTANCE_MM = 500.0;   // 例如相机安装距木板平面 500mm

// ---- 棋盘格尺寸（标定时使用，本 demo 不直接使用，仅供参考） ----
// static constexpr float CHESS_SQUARE_SIZE_MM = 25.0f;
// static constexpr int   CHESS_INNER_W = 9;   // 内角点数 宽
// static constexpr int   CHESS_INNER_H = 6;   // 内角点数 高

// ---- 输入图像路径 ----
static const char* IMAGE_PATH = "Image_muban.jpg";

// ============================================================
// 2. 图像分割 — 提取木板区域
// ============================================================

/**
 * 从图像中分割出木板主体，返回最大轮廓
 *
 * 策略: OTSU 二值化（木板浅色/背景深色）→ 形态学清理 → 找最大轮廓
 * 若 OTSU 效果不佳，可调整阈值偏移量 THRESH_OFFSET
 */
static std::vector<cv::Point> segmentBoard(const cv::Mat& bgr, bool debug = false) {
    // 灰度化
    cv::Mat gray;
    cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);

    // 高斯模糊去噪
    cv::Mat blurred;
    cv::GaussianBlur(gray, blurred, cv::Size(5, 5), 0);

    // OTSU 二值化 — 木板浅色为前景(白)，背景深色为背景(黑)
    cv::Mat binary;
    cv::threshold(blurred, binary, 0, 255, cv::THRESH_BINARY | cv::THRESH_OTSU);

    // 形态学闭运算 — 填充木板内部纹理造成的小孔洞
    cv::Mat kernel = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(7, 7));
    cv::morphologyEx(binary, binary, cv::MORPH_CLOSE, kernel);

    // 形态学开运算 — 去除边缘毛刺 / 细小噪点
    cv::morphologyEx(binary, binary, cv::MORPH_OPEN, kernel);

    if (debug) {
        cv::imwrite("debug_binary.jpg", binary);
        std::cout << "[Debug] 二值化结果已保存: debug_binary.jpg" << std::endl;
    }

    // 找所有轮廓
    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(binary, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

    if (contours.empty()) {
        std::cerr << "[Error] 未找到任何轮廓，图像可能全黑或全白" << std::endl;
        return {};
    }

    // 取面积最大的轮廓 — 即木板主体
    auto largest = std::max_element(contours.begin(), contours.end(),
        [](const std::vector<cv::Point>& a, const std::vector<cv::Point>& b) {
            return cv::contourArea(a) < cv::contourArea(b);
        });

    std::cout << "[Segment] 找到 " << contours.size() << " 个轮廓，"
              << "最大轮廓面积: " << cv::contourArea(*largest) << " px^2" << std::endl;

    return *largest;
}

// ============================================================
// 3. 像素坐标 → 世界坐标（pinhole 模型，假设木板在 Z 平面上）
// ============================================================

/**
 * 将图像像素点 (u, v) 投影到世界平面 Z = DISTANCE_MM 上
 *
 * pinhole 模型 (忽略畸变时):
 *   X = (u - cx) * Z / fx
 *   Y = (v - cy) * Z / fy
 *   Z = DISTANCE_MM
 *
 * 注意: 此模型假设相机光轴垂直于木板平面。
 *       若相机有倾斜，需先用 getPerspectiveTransform 做透视校正。
 */
static cv::Point3d pixelToWorld(const cv::Point2d& px, const cv::Mat& K, double Z) {
    double fx = K.at<double>(0, 0);
    double fy = K.at<double>(1, 1);
    double cx = K.at<double>(0, 2);
    double cy = K.at<double>(1, 2);

    double X = (px.x - cx) * Z / fx;
    double Y = (px.y - cy) * Z / fy;
    return {X, Y, Z};
}

// ============================================================
// 4. 主逻辑
// ============================================================

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "  木板长宽测量 Demo" << std::endl;
    std::cout << "========================================" << std::endl;

    // ---- 4a. 加载标定参数 ----
    cv::Mat K = cameraMatrix();
    cv::Mat D = distCoeffs();

    std::cout << "\n[标定参数]" << std::endl;
    std::cout << "  内参矩阵 K:\n" << K << std::endl;
    std::cout << "  畸变系数 D: " << D << std::endl;
    std::cout << "  物距 Z: " << DISTANCE_MM << " mm" << std::endl;

    // ---- 4b. 读取图像 ----
    cv::Mat raw = cv::imread(IMAGE_PATH);
    if (raw.empty()) {
        std::cerr << "[Error] 无法读取图像: " << IMAGE_PATH << std::endl;
        return -1;
    }
    std::cout << "\n[图像] " << IMAGE_PATH
              << " | 尺寸: " << raw.cols << "×" << raw.rows << std::endl;

    // ---- 4c. 畸变矫正 ----
    cv::Mat undistorted;
    cv::undistort(raw, undistorted, K, D);
    std::cout << "[矫正] 畸变矫正完成" << std::endl;

    // ---- 4d. 分割 → 提取木板轮廓 ----
    auto board_contour = segmentBoard(undistorted, /*debug=*/true);
    if (board_contour.empty()) {
        std::cerr << "[Error] 未能提取木板轮廓" << std::endl;
        return -2;
    }

    // ---- 4e. 最小外接旋转矩形 ----
    cv::RotatedRect rrect = cv::minAreaRect(board_contour);
    // rrect.size 返回 (width, height)，width < height 不一定成立，需要排序
    float rw = rrect.size.width;   // 像素宽度
    float rh = rrect.size.height;  // 像素高度
    float angle = rrect.angle;

    // 确保 width >= height 以便理解（约定 width 为较长边）
    if (rw < rh) {
        std::swap(rw, rh);
        // angle 调整: OpenCV 的 angle 是 width 边与水平轴的夹角 [-90, 0)
        // swap 后原始 width→height 概念互换，angle 含义也随之变化
        // 这里仅做显示用，不做严格还原
    }

    std::cout << "\n[旋转矩形 - 像素]" << std::endl;
    std::cout << "  width:  " << rw << " px" << std::endl;
    std::cout << "  height: " << rh << " px" << std::endl;
    std::cout << "  angle:  " << angle << "°" << std::endl;
    std::cout << "  center: (" << rrect.center.x << ", " << rrect.center.y << ")" << std::endl;

    // ---- 4f. 获取 4 个角点 → 投影到世界坐标 → 计算实际长宽 ----
    cv::Point2f corners[4];
    rrect.points(corners);   // 按顺序返回 4 个角点（左下→左上→右上→右下，可能旋转后不同）

    // 角点按顺序两两对应矩形的边:
    //   edge0 = corners[0] → corners[1]
    //   edge1 = corners[1] → corners[2]
    //   edge2 = corners[2] → corners[3]
    //   edge3 = corners[3] → corners[0]
    // 矩形的 width 和 height 对应两对不同长度的边

    std::vector<cv::Point3d> world_corners;
    std::cout << "\n[角点世界坐标 (mm)]" << std::endl;
    for (int i = 0; i < 4; ++i) {
        auto w = pixelToWorld(cv::Point2d(corners[i].x, corners[i].y), K, DISTANCE_MM);
        world_corners.push_back(w);
        std::cout << "  corner[" << i << "]: pixel("
                  << corners[i].x << ", " << corners[i].y << ") → world("
                  << std::fixed << std::setprecision(1)
                  << w.x << ", " << w.y << ") mm" << std::endl;
    }

    // 计算边长（mm）
    auto euclidean = [](const cv::Point3d& a, const cv::Point3d& b) -> double {
        double dx = a.x - b.x;
        double dy = a.y - b.y;
        return std::sqrt(dx * dx + dy * dy);
    };

    double edge_len[4];
    edge_len[0] = euclidean(world_corners[0], world_corners[1]);
    edge_len[1] = euclidean(world_corners[1], world_corners[2]);
    edge_len[2] = euclidean(world_corners[2], world_corners[3]);
    edge_len[3] = euclidean(world_corners[3], world_corners[0]);

    // 矩形对边应相等；取均值作为长/宽
    // edge0 和 edge2 是对边；edge1 和 edge3 是对边
    double side_a = (edge_len[0] + edge_len[2]) / 2.0;  // 一对边
    double side_b = (edge_len[1] + edge_len[3]) / 2.0;  // 另一对边

    // 确保 long_side >= short_side
    double long_side  = std::max(side_a, side_b);
    double short_side = std::min(side_a, side_b);

    std::cout << "\n========================================" << std::endl;
    std::cout << "  木板实际尺寸 (世界坐标)" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "  长 (long):   " << std::fixed << std::setprecision(1)
              << long_side << " mm  (" << long_side / 10.0 << " cm)" << std::endl;
    std::cout << "  宽 (short):  " << short_side << " mm  ("
              << short_side / 10.0 << " cm)" << std::endl;
    std::cout << "  像素长:      " << rw << " px" << std::endl;
    std::cout << "  像素宽:      " << rh << " px" << std::endl;
    std::cout << "  换算比例:    " << long_side / rw << " mm/px" << std::endl;
    std::cout << "\n(以上尺寸基于物距 Z=" << DISTANCE_MM
              << " mm，若与实际不符请调整相机安装高度)" << std::endl;

    // ---- 4g. 可视化 ----
    cv::Mat vis = undistorted.clone();

    // 画轮廓
    std::vector<std::vector<cv::Point>> contours_vis = {board_contour};
    cv::drawContours(vis, contours_vis, 0, cv::Scalar(0, 255, 0), 2);

    // 画旋转矩形
    cv::Point2f rcorners[4];
    rrect.points(rcorners);
    for (int i = 0; i < 4; ++i) {
        cv::line(vis, rcorners[i], rcorners[(i + 1) % 4], cv::Scalar(255, 0, 0), 2);
        cv::circle(vis, rcorners[i], 5, cv::Scalar(0, 0, 255), -1);
    }

    // 画中心
    cv::circle(vis, rrect.center, 6, cv::Scalar(0, 255, 255), -1);

    // 标注尺寸
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1)
        << long_side << " × " << short_side << " mm";
    cv::putText(vis, oss.str(),
                cv::Point(rrect.center.x - 130, rrect.center.y - 15),
                cv::FONT_HERSHEY_SIMPLEX, 0.8, cv::Scalar(0, 255, 255), 2);

    // 保存结果
    cv::imwrite("output_widthheight.jpg", vis);
    std::cout << "\n[结果] 可视化已保存: output_widthheight.jpg" << std::endl;

    // 显示
    cv::namedWindow("Wood Board Measurement", cv::WINDOW_NORMAL);
    cv::resizeWindow("Wood Board Measurement", 1200, 900);
    cv::imshow("Wood Board Measurement", vis);
    std::cout << "[显示] 按任意键退出..." << std::endl;
    cv::waitKey(0);
    cv::destroyAllWindows();

    return 0;
}
