/**
 * 木板瑕疵检测 v2.0 — TensorRT-YOLO 推理 + 海康相机 + PLC
 *
 * 流程: PLC ──TCP──→ Nano ──软触发──→ 相机 ──图像──→ AI检测 ──TCP──→ PLC
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

#include "config.h"
#include "camera.h"
#include "infer.h"
#include "postprocessor.h"
#include "plc_link.h"

static std::atomic<bool> running{true};

// ============================================================
// 全局指针 + 崩溃清理
// ============================================================
static HikvisionCamera* g_cam   = nullptr;
static PlcLink*         g_plc   = nullptr;

static void cleanup_all() {
    if (g_cam && g_cam->isRunning()) {
        std::cerr << "\n[CrashGuard] 强制释放相机..." << std::endl;
        g_cam->stop();
    }
    if (g_plc) {
        g_plc->stop();
    }
}

static void on_terminate() {
    std::cerr << "\n[CrashGuard] std::terminate 触发" << std::endl;
    cleanup_all();
    std::abort();
}

static void on_signal(int sig) {
    const char* name = "UNKNOWN";
    switch (sig) {
        case SIGINT:  name = "SIGINT";  break;
        case SIGTERM: name = "SIGTERM"; break;
        case SIGSEGV: name = "SIGSEGV"; break;
        case SIGABRT: name = "SIGABRT"; break;
    }

    if (sig == SIGINT || sig == SIGTERM) {
        std::cerr << "\n[CrashGuard] " << name << " 收到，正在退出..." << std::endl;
        running = false;
        cleanup_all();
        return;
    }

    std::cerr << "\n[CrashGuard] " << name << " 异常信号，尝试清理..." << std::endl;
    cleanup_all();
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

// ============================================================
// FPS 统计
// ============================================================
struct FPS {
    std::deque<double> h;
    void add(double ms) { h.push_back(ms); if (h.size() > 100) h.pop_front(); }
    double val() const {
        if (h.empty()) return 0;
        double avg = std::accumulate(h.begin(), h.end(), 0.0) / h.size();
        return avg > 0 ? 1000.0 / avg : 0;
    }
};

// ============================================================
// CLAHE 增强（LAB L通道）
// ============================================================
static cv::Mat enhance(const cv::Mat& f, float clip, int tile) {
    cv::Mat lab, out;
    cv::cvtColor(f, lab, cv::COLOR_BGR2Lab);
    std::vector<cv::Mat> ch(3);
    cv::split(lab, ch);
    cv::createCLAHE(clip, cv::Size(tile, tile))->apply(ch[0], ch[0]);
    cv::merge(ch, lab);
    cv::cvtColor(lab, out, cv::COLOR_Lab2BGR);
    return out;
}

// ============================================================
// 主函数
// ============================================================
int main() {
    // ---- 注册崩溃清理 ----
    std::set_terminate(on_terminate);
    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGSEGV, on_signal);
    std::signal(SIGABRT, on_signal);

    // ---- 1. 相机（软触发模式） ----
    HikvisionCamera cam;
    g_cam = &cam;

    try {
        if (!cam.connectByIP(Config::CAMERA_IP)) {
            std::cerr << "相机连接失败" << std::endl;
            return -1;
        }
        cam.start(Config::CAMERA_WIDTH, Config::CAMERA_HEIGHT,
                  Config::CAMERA_EXPOSURE, Config::CAMERA_GAIN, Config::CAMERA_TRIGGER);

        // ---- 2. 推理引擎 ----
        InferEngine infer;
        if (!infer.load(Config::ENGINE_PATH)) {
            std::cerr << "引擎加载失败" << std::endl;
            cam.stop();
            return -2;
        }

        // ---- 3. 后处理 ----
        Postprocessor post(Config::CONF_THRESHOLD, Config::CLASSES);

        // ---- 4. PLC TCP Server ----
        PlcLink plc(Config::PLC_TCP_PORT);
        g_plc = &plc;
        if (!plc.start()) {
            std::cerr << "PLC TCP Server 启动失败" << std::endl;
            cam.stop();
            return -3;
        }

        // ---- 5. 主循环（PLC 事件驱动） ----
        FPS fps;
        uint64_t total = 0, ng_total = 0;
        auto t0 = std::chrono::steady_clock::now();

        std::cout << "系统就绪（等待 PLC 触发指令...）\n" << std::endl;

        while (running) {
            // 等待 PLC 发来触发指令（每秒检查 running 标志）
            if (!plc.waitTrigger(1000)) {
                if (!plc.isConnected()) {
                    std::cout << "[PLC] 等待连接中..." << std::endl;
                }
                continue;
            }

            auto t1 = std::chrono::steady_clock::now();

            // 软触发相机拍照
            cam.softwareTrigger();

            // 读取图像
            cv::Mat frame;
            int retry = 0;
            while (retry < 10 && running) {
                frame = cam.read();
                if (!frame.empty()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                retry++;
            }
            if (frame.empty()) {
                std::cerr << "[Camera] 触发后未获取到图像" << std::endl;
                continue;
            }

            // CLAHE 增强
            cv::Mat img = enhance(frame, 2.0f, 8);

            // 推理
            auto res = infer.detect(img);

            // 后处理 + 画框
            cv::Size sz(img.cols, img.rows);
            auto defects = post.process(res, img, sz);

            // NG 判定
            bool is_ng = false;
            for (auto& d : defects)
                if (d.level == "ng") { is_ng = true; ng_total++; break; }

            // 发送结果给 PLC
            if (is_ng) {
                plc.sendNG();
                std::cout << "[PLC] → NG" << std::endl;
            } else {
                plc.sendOK();
                std::cout << "[PLC] → OK" << std::endl;
            }

            if (!defects.empty() && Config::SAVE_IMAGES && is_ng)
                post.save(img, defects, Config::OUTPUT_DIR);

            // 显示
            if (Config::SHOW_DISPLAY) {
                cv::putText(img, is_ng ? "NG" : "OK", {10, 30},
                            cv::FONT_HERSHEY_SIMPLEX, 1,
                            is_ng ? cv::Scalar(0,0,255) : cv::Scalar(0,255,0), 2);
                cv::imshow("Wood Defect Detection", img);
                if ((cv::waitKey(1) & 0xFF) == 27) { running = false; break; }
            }

            // 统计
            auto t2 = std::chrono::steady_clock::now();
            fps.add(std::chrono::duration<double, std::milli>(t2 - t1).count());
            total++;

            if (total % 50 == 0)
                std::cout << "FPS:" << std::fixed << std::setprecision(1) << fps.val()
                          << " | 检测:" << total << " | NG:" << ng_total << std::endl;
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
        cam.stop();
        return -5;
    } catch (...) {
        std::cerr << "未知异常" << std::endl;
        if (g_plc) g_plc->stop();
        cam.stop();
        return -6;
    }
}
