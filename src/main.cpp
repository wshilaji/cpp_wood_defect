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

#include "config.h"
#include "camera.h"
#include "infer.h"
#include "postprocessor.h"

static std::atomic<bool> running{true};

void on_signal(int) { running = false; }

struct FPS {
    std::deque<double> h;
    void add(double ms) { h.push_back(ms); if (h.size() > 100) h.pop_front(); }
    double val() const {
        if (h.empty()) return 0;
        double avg = std::accumulate(h.begin(), h.end(), 0.0) / h.size();
        return avg > 0 ? 1000.0 / avg : 0;
    }
};

// CLAHE 增强（LAB L通道）
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

int main() {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    // 1. 相机
    HikvisionCamera cam;

    try {
        if (!cam.connectByIP(Config::CAMERA_IP)) {
            std::cerr << "相机连接失败" << std::endl;
            return -1;
        }
        cam.start(Config::CAMERA_WIDTH, Config::CAMERA_HEIGHT,
                  Config::CAMERA_EXPOSURE, Config::CAMERA_GAIN, Config::CAMERA_TRIGGER);

        // 2. 推理引擎
        InferEngine infer;
        if (!infer.load(Config::ENGINE_PATH)) {
            std::cerr << "引擎加载失败" << std::endl;
            cam.stop();
            return -2;
        }

        // 3. 后处理
        Postprocessor post(Config::CONF_THRESHOLD, Config::CLASSES);

        // 4. 主循环
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
