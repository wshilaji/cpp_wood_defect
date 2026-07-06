/**
 * 木板瑕疵检测系统 - 主程序
 *
 * 运行于 NVIDIA Jetson Nano
 * 流水线实时检测木板表面缺陷: 节疤/裂纹/孔洞/污渍/划痕/边角破损/腐朽
 *
 * 数据流:
 *   海康工业相机 → MVS SDK 采集 → Bayer→BGR
 *   → CLAHE 增强 → TensorRT FP16 推理 → NMS 后处理
 *   → 瑕疵判定 → 显示/保存/PLC剔除
 *
 * 编译:
 *   mkdir build && cd build
 *   cmake -DCMAKE_BUILD_TYPE=Release ..
 *   make -j$(nproc)
 *
 * 运行:
 *   ./wood_defect_detector
 */

#include <opencv2/opencv.hpp>
#include <iostream>
#include <iomanip>
#include <cstring>
#include <csignal>
#include <atomic>
#include <chrono>
#include <thread>
#include <deque>
#include <numeric>
#include <fstream>

#include "config.h"
#include "camera.h"
#include "preprocessor.h"
#include "trt_engine.h"
#include "postprocessor.h"

// ============================================================
// 全局状态
// ============================================================
static std::atomic<bool> g_running{true};

void signalHandler(int sig) {
    std::cout << "\n[Main] 收到信号 " << sig << ", 正在安全退出..." << std::endl;
    g_running.store(false);
}

// ============================================================
// FPS 计数器
// ============================================================
class FPSCounter {
public:
    void tick(double elapsed_ms) {
        _history.push_back(elapsed_ms);
        if (_history.size() > 100) _history.pop_front();
    }

    double fps() const {
        if (_history.empty()) return 0.0;
        double avg_ms = std::accumulate(_history.begin(), _history.end(), 0.0)
                        / _history.size();
        return (avg_ms > 0.0) ? 1000.0 / avg_ms : 0.0;
    }

    double avgLatencyMs() const {
        if (_history.empty()) return 0.0;
        return std::accumulate(_history.begin(), _history.end(), 0.0)
               / _history.size();
    }

private:
    std::deque<double> _history;
};

// ============================================================
// 首次运行: 检测 .trt 引擎是否存在，不存在则从 ONNX 构建
// ============================================================
bool ensureEngine(TensorRTEngine& engine) {
    std::ifstream trt_file(Config::MODEL_PATH, std::ios::binary);
    if (trt_file.good()) {
        trt_file.close();
        return engine.loadEngine(Config::MODEL_PATH);
    }

    // 引擎不存在，从 ONNX 构建
    std::cout << "\n[Main] ===================================" << std::endl;
    std::cout << "[Main] 未找到 TensorRT 引擎文件" << std::endl;
    std::cout << "[Main] 正在从 ONNX 构建（首次运行, 约需2-5分钟）..." << std::endl;
    std::cout << "[Main] ===================================\n" << std::endl;

    bool ok = engine.buildFromONNX(
        Config::ONNX_PATH,
        Config::MODEL_PATH,
        Config::USE_FP16,
        1,
        static_cast<size_t>(Config::MAX_WORKSPACE_SIZE)
    );

    if (ok) {
        std::cout << "\n[Main] TensorRT 引擎构建完成！" << std::endl;
    }
    return ok;
}

// ============================================================
// 打印启动横幅
// ============================================================
void printBanner() {
    std::cout << "\n========================================" << std::endl;
    std::cout << "  木 板 瑕 疵 检 测 系 统  v1.0" << std::endl;
    std::cout << "  NVIDIA Jetson Nano | C++ | TensorRT" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "  类别: ";
    for (size_t i = 1; i < Config::CLASS_NAMES.size(); ++i) {
        if (i > 1) std::cout << ", ";
        std::cout << Config::CLASS_NAMES[i];
    }
    std::cout << std::endl;
    std::cout << "========================================\n" << std::endl;
}

// ============================================================
// 主入口
// ============================================================
int main(int argc, char* argv[]) {
    // ---- 注册信号处理 ----
    std::signal(SIGINT,  signalHandler);
    std::signal(SIGTERM, signalHandler);

    printBanner();

    // ========================================================
    // 1. 初始化海康工业相机
    // ========================================================
    HikvisionCamera camera;

    // 按序列号 / IP / 自定义名称连接（优先级: 序列号 > IP > 自定义名称）
    bool camera_ok = false;
    if (strlen(Config::CAMERA_SERIAL) > 0) {
        camera_ok = camera.connectBySerial(Config::CAMERA_SERIAL);
    } else if (strlen(Config::CAMERA_IP) > 0) {
        camera_ok = camera.connectByIP(Config::CAMERA_IP);
    } else if (strlen(Config::CAMERA_USER_ID) > 0) {
        camera_ok = camera.connectByUserID(Config::CAMERA_USER_ID);
    } else {
        // 不指定则连接第一个在线设备
        camera_ok = camera.connectByIP("");  // 空 IP 自动匹配首个设备
    }

    if (!camera_ok) {
        std::cerr << "[Main] FATAL: 海康相机连接失败" << std::endl;
        return -1;
    }

    // 启动采集（设置触发模式、曝光、增益）
    if (!camera.start(Config::CAMERA_WIDTH, Config::CAMERA_HEIGHT,
                       Config::CAMERA_EXPOSURE_US,
                       Config::CAMERA_GAIN_DB,
                       Config::CAMERA_TRIGGER_MODE)) {
        std::cerr << "[Main] FATAL: 相机取流启动失败" << std::endl;
        return -1;
    }

    // ========================================================
    // 2. 初始化 TensorRT 引擎
    // ========================================================
    TensorRTEngine engine;
    if (!ensureEngine(engine)) {
        std::cerr << "[Main] FATAL: TensorRT 引擎初始化失败" << std::endl;
        camera.stop();
        return -2;
    }

    // ========================================================
    // 3. 初始化预处理器
    // ========================================================
    Preprocessor preprocessor(Config::INPUT_WIDTH,
                              Config::INPUT_HEIGHT,
                              Config::ENABLE_CLAHE,
                              Config::CLAHE_CLIP_LIMIT,
                              Config::CLAHE_TILE_SIZE);

    // 预分配输入 tensor 内存（复用避免反复分配）
    size_t tensor_size = Config::INPUT_WIDTH * Config::INPUT_HEIGHT * Config::INPUT_CHANNELS;
    std::vector<float> input_tensor(tensor_size, 0.0f);

    // ========================================================
    // 4. 初始化后处理器
    // ========================================================
    Postprocessor postprocessor(Config::CONF_THRESHOLD,
                                Config::NMS_THRESHOLD,
                                Config::CLASS_NAMES);

    // ========================================================
    // 5. 主检测循环
    // ========================================================
    std::cout << "[Main] 系统就绪，开始检测...\n" << std::endl;

    FPSCounter fps_counter;
    uint64_t   total_frames   = 0;
    uint64_t   defect_frames  = 0;
    uint64_t   ng_count       = 0;
    auto       start_time     = std::chrono::steady_clock::now();

    std::vector<float*> outputs;       // 模型输出指针（复用）
    std::vector<size_t> output_sizes;  // 各输出大小

    // 获取输出维度信息
    const auto& out_shapes = engine.getOutputShapes();
    for (const auto& shape : out_shapes) {
        size_t s = sizeof(float);
        for (int d : shape) s *= d;
        output_sizes.push_back(s);
    }

    while (g_running.load()) {
        auto frame_start = std::chrono::steady_clock::now();

        // ---- Step 1: 采集帧 ----
        cv::Mat frame = camera.read();
        if (frame.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }

        // ---- Step 2: 预处理 ----
        cv::Mat display_frame;
        preprocessor.process(frame, input_tensor.data(), display_frame);

        // ---- Step 3: TensorRT 推理 ----
        if (!engine.infer(input_tensor.data(), outputs, 1)) {
            std::cerr << "[Main] 推理失败，跳过此帧" << std::endl;
            continue;
        }

        // ---- Step 4: 后处理 + 判定 ----
        auto defects = postprocessor.process(
            outputs.data(),
            static_cast<int>(outputs.size()),
            output_sizes,
            display_frame,
            Config::INPUT_WIDTH,
            Config::INPUT_HEIGHT);

        // ---- Step 5: 决策输出 ----
        bool has_ng   = false;
        bool has_warn = false;

        for (const auto& d : defects) {
            if (d.severity == "ng")   { has_ng   = true; break; }
            if (d.severity == "warn") { has_warn = true; }
        }

        if (has_ng) {
            ng_count++;
            // TODO: 通过 GPIO/串口 发送剔除信号给 PLC
            // gpioWrite(REJECT_PIN, HIGH);
            // delay_us(1000);
            // gpioWrite(REJECT_PIN, LOW);
        }

        if (!defects.empty()) {
            defect_frames++;
            if (Config::SAVE_DEFECT_IMAGES && has_ng) {
                postprocessor.saveDefectImage(display_frame, defects,
                                              Config::OUTPUT_DIR);
            }

            // 打印缺陷信息
            std::cout << "[DETECT] ";
            for (size_t i = 0; i < defects.size(); ++i) {
                if (i > 0) std::cout << " | ";
                std::cout << defects[i].class_name
                          << "(" << std::fixed << std::setprecision(2)
                          << defects[i].confidence << ")"
                          << " [" << defects[i].severity << "]";
            }
            std::cout << std::endl;
        }

        // ---- Step 6: 显示 ----
        if (Config::SHOW_DISPLAY) {
            // 叠加状态信息
            std::ostringstream status_oss;
            status_oss << "Status: " << (has_ng ? "NG" : has_warn ? "WARN" : "OK");
            cv::Scalar status_color = has_ng ? cv::Scalar(0, 0, 255)
                                     : has_warn ? cv::Scalar(0, 255, 255)
                                     : cv::Scalar(0, 255, 0);

            cv::putText(display_frame, status_oss.str(),
                        cv::Point(10, 30),
                        cv::FONT_HERSHEY_SIMPLEX, 1.0,
                        status_color, 2);

            std::ostringstream fps_oss;
            fps_oss << "FPS: " << std::fixed << std::setprecision(1)
                    << fps_counter.fps();
            cv::putText(display_frame, fps_oss.str(),
                        cv::Point(10, 60),
                        cv::FONT_HERSHEY_SIMPLEX, 0.6,
                        cv::Scalar(255, 255, 255), 1);

            cv::imshow("Wood Defect Detection", display_frame);

            int key = cv::waitKey(1) & 0xFF;
            if (key == 'q' || key == 27) {  // q 或 ESC
                g_running.store(false);
                std::cout << "[Main] 用户退出" << std::endl;
                break;
            }
        }

        // ---- FPS 统计 ----
        auto frame_end = std::chrono::steady_clock::now();
        double elapsed_ms = std::chrono::duration<double, std::milli>(
                                frame_end - frame_start).count();
        fps_counter.tick(elapsed_ms);
        total_frames++;

        if (Config::PRINT_FPS && total_frames % Config::FPS_LOG_INTERVAL == 0) {
            std::cout << "[Main] FPS: " << std::fixed << std::setprecision(1)
                      << fps_counter.fps()
                      << " | 平均延迟: " << std::setprecision(1)
                      << fps_counter.avgLatencyMs() << "ms"
                      << " | 已处理: " << total_frames << "帧"
                      << " | NG: " << ng_count
                      << std::endl;
        }
    }

    // ========================================================
    // 6. 清理 & 统计
    // ========================================================
    auto end_time = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(end_time - start_time).count();

    camera.stop();
    cv::destroyAllWindows();

    std::cout << "\n========================================" << std::endl;
    std::cout << "  系统已停止" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "  运行时长:    " << std::fixed << std::setprecision(1)
              << elapsed << "s" << std::endl;
    std::cout << "  处理帧数:    " << total_frames << std::endl;
    std::cout << "  平均 FPS:    " << std::setprecision(1)
              << (total_frames / std::max(elapsed, 0.001)) << std::endl;
    std::cout << "  瑕疵帧数:    " << defect_frames << std::endl;
    std::cout << "  NG 次数:     " << ng_count << std::endl;
    std::cout << "========================================" << std::endl;

    return 0;
}
