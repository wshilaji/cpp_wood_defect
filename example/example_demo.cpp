/**
 * example_demo.cpp — 相机 + YOLO11n (COCO) 检测示例
 *
 * 规则:
 *   - person 数量 > 10  → NG
 *   - car 检测到任意数量 → NG
 *   - 否则              → OK
 *
 * 运行: ./example_demo
 * 触发: 终端按回车 或 echo "TRIGGER" | nc <ip> 5000
 */
#include <opencv2/opencv.hpp>
#include <iostream>
#include <csignal>
#include <atomic>
#include <chrono>
#include <thread>
#include <deque>
#include <numeric>
#include <exception>
#include <sstream>
#include <fstream>
#include <poll.h>
#include <unistd.h>

#include "camera.h"
#include "trtyolo.hpp"
#include "plc_link.h"

// ============================================================
// 配置（example 专用，硬编码）
// ============================================================
constexpr const char* ENGINE_PATH = "example/models/yolo26n.engine";
constexpr const char* LABEL_PATH  = "example/labels.txt";
constexpr const char* CAMERA_IP   = "192.168.1.10";
constexpr int         CAMERA_W    = 640;
constexpr int         CAMERA_H    = 640;
constexpr float       EXPOSURE_US = 5000.0f;
constexpr float       GAIN_DB     = 0.0f;
constexpr int         PLC_PORT    = 5000;

// 判定规则
constexpr int PERSON_MAX = 10;   // person 超过此数量 → NG
// car > 0 → NG

static std::atomic<bool> running{true};

// ============================================================
// 崩溃清理
// ============================================================
static HikvisionCamera* g_cam = nullptr;
static PlcLink*         g_plc = nullptr;

static void cleanup() {
    if (g_plc) g_plc->stop();
    if (g_cam && g_cam->isRunning()) {
        std::cerr << "\n[CrashGuard] 释放相机..." << std::endl;
        g_cam->stop();
    }
}

static void on_terminate() {
    std::cerr << "\n[CrashGuard] std::terminate" << std::endl;
    cleanup();
    std::abort();
}

static void on_signal(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        std::cerr << "\n[CrashGuard] 信号" << sig << "，退出..." << std::endl;
        running = false;
        cleanup();
        return;
    }
    std::cerr << "\n[CrashGuard] 异常信号" << sig << std::endl;
    cleanup();
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

// ============================================================
// 读取标签文件
// ============================================================
static std::vector<std::string> loadLabels(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) throw std::runtime_error("无法打开标签文件: " + path);
    std::vector<std::string> labels;
    std::string line;
    while (std::getline(f, line)) labels.push_back(line);
    std::cout << "[Labels] 加载 " << labels.size() << " 个类别" << std::endl;
    return labels;
}

// ============================================================
// 主函数
// ============================================================
int main() {
    std::set_terminate(on_terminate);
    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGSEGV, on_signal);
    std::signal(SIGABRT, on_signal);

    try {
        // ---- 1. 加载标签 ----
        auto labels = loadLabels(LABEL_PATH);

        // ---- 2. 相机（连续采集模式） ----
        HikvisionCamera cam;
        g_cam = &cam;
        if (!cam.connectByIP(CAMERA_IP)) {
            std::cerr << "相机连接失败" << std::endl;
            return -1;
        }
        // 连续采集模式（trigger_mode=0），FPS测试用
        cam.start(CAMERA_W, CAMERA_H, EXPOSURE_US, GAIN_DB, 0);

        // ---- 3. 推理引擎 ----
        std::cout << "[Model] 加载 " << ENGINE_PATH << " ..." << std::endl;
        trtyolo::InferOption opt;
        opt.enableSwapRB();
        trtyolo::DetectModel model(ENGINE_PATH, opt);
        std::cout << "[Model] 加载完成" << std::endl;

        // ---- 4. PLC TCP Server ----
        PlcLink plc(PLC_PORT);
        g_plc = &plc;
        if (!plc.start()) {
            std::cerr << "PLC TCP Server 启动失败" << std::endl;
            cam.stop();
            return -3;
        }

        // ---- 5. 主循环 ----
        uint64_t total = 0, ng_total = 0;
        auto t0 = std::chrono::steady_clock::now();

        std::cout << "\n========================================" << std::endl;
        std::cout << "  系统就绪 — 按回车或 TCP 触发拍照" << std::endl;
        std::cout << "  规则: person>" << PERSON_MAX << " → NG | car>0 → NG" << std::endl;
        std::cout << "========================================\n" << std::endl;

        while (running) {
            // 组合等待: 终端输入 或 TCP 触发
            bool triggered = plc.waitTrigger(500);

            // 检查终端输入（非阻塞）
            if (!triggered) {
                struct pollfd pfd;
                pfd.fd = STDIN_FILENO;
                pfd.events = POLLIN;
                if (poll(&pfd, 1, 500) > 0) {
                    std::string line;
                    std::getline(std::cin, line);
                    std::cout << "[Input] 手动触发" << std::endl;
                    triggered = true;
                }
            }

            if (!triggered) continue;

            // 从连续流中取一帧
            cv::Mat frame;
            int retry = 0;
            while (retry < 20 && running) {
                frame = cam.read();
                if (!frame.empty()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                retry++;
            }
            if (frame.empty()) { std::cerr << "[Camera] 无帧" << std::endl; continue; }

            auto t1 = std::chrono::steady_clock::now();

            // 推理
            trtyolo::Image img(frame.data, frame.cols, frame.rows);
            auto res = model.predict(img);

            // 统计 person / car
            int person_cnt = 0;
            int car_cnt    = 0;
            for (int i = 0; i < res.num; ++i) {
                int cls = res.classes[i];
                if (cls == 0) person_cnt++;   // COCO class 0 = person
                if (cls == 2) car_cnt++;       // COCO class 2 = car
            }

            bool is_ng = (person_cnt > PERSON_MAX) || (car_cnt > 0);

            // 发送结果
            if (is_ng) plc.sendNG(); else plc.sendOK();

            auto t2 = std::chrono::steady_clock::now();
            double ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
            total++;

            // 日志
            std::cout << "[" << total << "] "
                      << "person:" << person_cnt << " car:" << car_cnt
                      << " → " << (is_ng ? "NG" : "OK")
                      << " (" << std::fixed << std::setprecision(0) << ms << "ms)" << std::endl;
            if (is_ng) ng_total++;

            // 可视化
            for (int i = 0; i < res.num; ++i) {
                const auto& b = res.boxes[i];
                int cls = res.classes[i];
                float score = res.scores[i];
                cv::Scalar color = (cls == 0) ? cv::Scalar(0, 255, 0) :   // person=绿
                                   (cls == 2) ? cv::Scalar(0, 0, 255) :   // car=红
                                                cv::Scalar(255, 255, 255);
                cv::rectangle(frame, cv::Point(b.left, b.top),
                              cv::Point(b.right, b.bottom), color, 2);
                cv::putText(frame, labels[cls] + " " + std::to_string(score).substr(0,4),
                            cv::Point(b.left, b.top - 5),
                            cv::FONT_HERSHEY_SIMPLEX, 0.5, color, 1);
            }

            cv::putText(frame, is_ng ? "NG" : "OK", {10, 30},
                        cv::FONT_HERSHEY_SIMPLEX, 1,
                        is_ng ? cv::Scalar(0,0,255) : cv::Scalar(0,255,0), 2);
            cv::imshow("Example Demo - YOLO11n", frame);
            if ((cv::waitKey(1) & 0xFF) == 27) { running = false; break; }
        }

        plc.stop();
        cam.stop();
        cv::destroyAllWindows();
        auto dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::cout << "\n停止 | 运行:" << (int)dt << "s | 检测:" << total
                  << " | NG:" << ng_total << std::endl;
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "异常: " << e.what() << std::endl;
        if (g_plc) g_plc->stop();
        if (g_cam) g_cam->stop();
        return -2;
    }
}
