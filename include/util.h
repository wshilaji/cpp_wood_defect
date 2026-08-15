#pragma once

/**
 * 通用工具 — 存图保护闸 / 存图辅助
 *
 * 存图保护：本次运行累计存图超过 1GB 就停存（防硬盘写满）。
 * 每次启动清零（进程级 inline 变量）；要按磁盘剩余空间保护可再加 df 检查。
 */

#include <cstdint>
#include <string>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <iostream>
#include <sys/stat.h>

#include <opencv2/opencv.hpp>

#include "config.h"

// ============================================================
// 存图容量保护闸
// ============================================================
inline constexpr uint64_t SAVE_CAP_BYTES = 1024ULL * 1024ULL * 1024ULL;   // 1GB
inline uint64_t g_savedBytes  = 0;       // 本次运行累计已存字节
inline bool     g_saveBlocked = false;   // 超限后置位，界面显示「存图已停」

/** 超 1GB 返回 false 并置 g_saveBlocked（主循环调 win.setSaveBlocked 提示界面） */
inline bool saveAllowed() {
    if (g_saveBlocked) return false;
    if (g_savedBytes >= SAVE_CAP_BYTES) {
        g_saveBlocked = true;
        std::cerr << "[SaveGuard] 累计存图超 1GB，停止存图（防硬盘写满）" << std::endl;
        return false;
    }
    return true;
}

/** 文件字节数（不存在/为空返回 0），用于累计 g_savedBytes */
inline uint64_t fileSizeBytes(const std::string& path) {
    if (path.empty()) return 0;
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return 0;
    return (uint64_t)st.st_size;
}

// ============================================================
// 保存原始图（按比例采样，比例由界面输入框控制）
// 必须在任何处理之前调用，因为后续 draw/measure 会原地改图
// ============================================================
inline std::string saveOriginalImage(const cv::Mat& frame) {
    if (!Config::SAVE_IMAGES || frame.empty()) return "";

    std::string dir = std::string(Config::OUTPUT_DIR) + "raw/";
    mkdir(dir.c_str(), 0755);

    // 文件名: RAW_YYYYmmdd_HHMMSS_xxx.jpg
    auto now = std::chrono::system_clock::now();
    auto t   = std::chrono::system_clock::to_time_t(now);
    auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                   now.time_since_epoch()) % 1000;
    std::ostringstream ss;
    ss << dir << "RAW_" << std::put_time(std::localtime(&t), "%Y%m%d_%H%M%S_")
       << std::setfill('0') << std::setw(3) << ms.count() << ".jpg";

    std::vector<int> jpg{cv::IMWRITE_JPEG_QUALITY, 85};   // 质量85，压一压存图大小
    cv::imwrite(ss.str(), frame, jpg);
    std::cout << "[Save] 原始图 → " << ss.str() << std::endl;
    return ss.str();
}
