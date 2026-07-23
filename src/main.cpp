/**
 * 木板瑕疵检测 v2.0 — TensorRT-YOLO 推理 + 海康相机 + PLC
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

#include "config.h"
#include "camera.h"
#include "infer.h"
#include "postprocessor.h"

static std::atomic<bool> running{true};

// ============================================================
// 全局指针 + 崩溃清理（无论怎么崩，都尝试释放相机）
// ============================================================
static HikvisionCamera* g_cam = nullptr;

static void cleanup_camera() {
    if (g_cam && g_cam->isRunning()) {
        std::cerr << "\n[CrashGuard] 强制释放相机句柄..." << std::endl;
        g_cam->stop();
    }
}

// std::terminate 触发时（未捕获异常 → std::terminate → std::abort）
static void on_terminate() {
    std::cerr << "\n[CrashGuard] std::terminate 触发" << std::endl;
    cleanup_camera();
    std::abort();  // 恢复默认行为
}

// 信号处理（SIGINT / SIGTERM / SIGSEGV / SIGABRT）
static void on_signal(int sig) {
    const char* name = "UNKNOWN";
    switch (sig) {
        case SIGINT:  name = "SIGINT(ctrl+c)";  break;
        case SIGTERM: name = "SIGTERM";          break;
        case SIGSEGV: name = "SIGSEGV";          break;
        case SIGABRT: name = "SIGABRT";          break;
    }

    if (sig == SIGINT || sig == SIGTERM) {
        // 优雅退出：先设标志让主循环停下来
        std::cerr << "\n[CrashGuard] " << name << " 收到，正在退出..." << std::endl;
        running = false;
        cleanup_camera();
        // 主循环检查到 running==false 后会正常走到 cam.stop()
        // cleanup_camera 里也调了一次 stop，多次调用是安全的
        return;
    }

    // SIGSEGV / SIGABRT：尽力清理（此时进程状态可能异常，但不试白不试）
    std::cerr << "\n[CrashGuard] " << name << " 异常信号，尝试清理相机..." << std::endl;
    cleanup_camera();
    // 恢复默认处理以生成 core dump
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
    // ---- 注册崩溃清理（std::terminate + 信号） ----
    std::set_terminate(on_terminate);
    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGSEGV, on_signal);
    std::signal(SIGABRT, on_signal);

    // ---- 1. 相机 ----
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

        // ---- 4. 主循环 ----
        FPS fps;
        uint64_t n = 0, ng = 0;
        auto t0 = std::chrono::steady_clock::now();

        std::cout << "系统就绪\n" << std::endl;

        while (running) {
            auto t1 = std::chrono::steady_clock::now();

            cv::Mat frame = cam.read();
            if (frame.empty()) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); continue; }

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
                if (d.level == "ng") { is_ng = true; ng++; break; }
            // TODO: PLC 剔除信号

            if (!defects.empty() && Config::SAVE_IMAGES && is_ng)
                post.save(img, defects, Config::OUTPUT_DIR);

            // 显示
            if (Config::SHOW_DISPLAY) {
                cv::putText(img, is_ng ? "NG" : "OK", {10, 30},
                            cv::FONT_HERSHEY_SIMPLEX, 1,
                            is_ng ? cv::Scalar(0,0,255) : cv::Scalar(0,255,0), 2);
                std::ostringstream ss;
                ss << "FPS:" << std::fixed << std::setprecision(1) << fps.val();
                cv::putText(img, ss.str(), {10, 60},
                            cv::FONT_HERSHEY_SIMPLEX, 0.6, {255,255,255}, 1);
                cv::imshow("Wood Defect Detection", img);
                if ((cv::waitKey(1) & 0xFF) == 27) { running = false; break; }
            }

            // 统计
            auto t2 = std::chrono::steady_clock::now();
            fps.add(std::chrono::duration<double, std::milli>(t2 - t1).count());
            n++;

            if (n % 100 == 0)
                std::cout << "FPS:" << std::fixed << std::setprecision(1) << fps.val()
                          << " | 帧:" << n << " | NG:" << ng << std::endl;
        }

        cam.stop();
        cv::destroyAllWindows();
        auto dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::cout << "\n停止 | 运行:" << (int)dt << "s | 帧:" << n
                  << " | FPS:" << (n / std::max(dt, 0.001))
                  << " | NG:" << ng << std::endl;
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "异常: " << e.what() << std::endl;
        cam.stop();
        return -3;
    } catch (...) {
        std::cerr << "未知异常" << std::endl;
        cam.stop();
        return -4;
    }
}
