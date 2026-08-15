/**
 * 木板瑕疵检测 v2.0 — TensorRT-YOLO 推理 + 海康相机 + PLC + Qt 界面
 *
 * 流程: PLC ──TCP──→ Nano ──软触发──→ 相机 ──图像──→ AI检测 ──TCP──→ PLC
 * 界面: Qt 窗口显示图像 + 统计 + 工人设置(jieba阈值/存图比例) + 相机调参
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
#include <iomanip>
#include <fstream>
#include <poll.h>
#include <unistd.h>

#include <QApplication>
#include <QString>

#include "config.h"
#include "camera.h"
#include "infer.h"
#include "postprocessor.h"
#include "plc_link.h"
#include "measure.h"
#include "perf.h"
#include "util.h"
#include "mainwindow.h"

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
// GPU 温度读取（Jetson，1分钟缓存）
// ============================================================
static float getGPUTemp() {
    static float cached = -1;
    static auto last = []() -> std::chrono::steady_clock::time_point {
        return std::chrono::steady_clock::time_point{};
    }();
    auto now = std::chrono::steady_clock::now();
    if (cached < 0 || std::chrono::duration<double>(now - last).count() > 60.0) {
        std::ifstream f("/sys/devices/virtual/thermal/thermal_zone1/temp");
        if (f.is_open()) {
            int raw;
            f >> raw;
            cached = raw / 1000.0f;
        }
        last = now;
    }
    return cached;
}

// ============================================================
// 主函数
// ============================================================
int main(int argc, char** argv) {
    // ---- 注册崩溃清理 ----
    std::set_terminate(on_terminate);
    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGSEGV, on_signal);
    std::signal(SIGABRT, on_signal);

    // ---- Qt 界面 ----
    QApplication app(argc, argv);
    MainWindow win;
    // 默认全屏 kiosk 模式：无边框 + 置顶 + 全屏，连桌面侧边栏/任务栏一起盖住
    win.setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    win.showFullScreen();

    // ---- 相机（软触发模式） ----
    HikvisionCamera cam;
    g_cam = &cam;

    try {
        if (!cam.connectByIP(Config::CAMERA_IP)) {
            std::cerr << "相机连接失败" << std::endl;
            return -1;
        }
        cam.start(Config::CAMERA_WIDTH, Config::CAMERA_HEIGHT,
                  Config::CAMERA_EXPOSURE, Config::CAMERA_GAIN, Config::CAMERA_TRIGGER);
        win.setCamRunning(true);

        // ---- 推理引擎 ----
        InferEngine infer;
        if (!infer.load(Config::ENGINE_PATH)) {
            std::cerr << "引擎加载失败" << std::endl;
            cam.stop();
            return -2;
        }
        win.setEngineReady(true);

        // ---- 后处理 ----
        Postprocessor post(Config::CONF_THRESHOLD, Config::CLASSES);

        // ---- PLC TCP Server ----
        PlcLink plc(Config::PLC_TCP_PORT);
        g_plc = &plc;
        if (!plc.start()) {
            std::cerr << "PLC TCP Server 启动失败" << std::endl;
            cam.stop();
            return -3;
        }

        // ---- 预热: 触发一次填满相机管线，避免首次拍照丢帧 ----
        {
            uint64_t since = cam.frameCount();
            cam.softwareTrigger();
            cam.readNewest(since, 500);   // 等预热帧到并丢弃
        }

        // ---- 主循环（PLC / 手动拍照 / 回车后门 触发） ----
        FPS fps;
        uint64_t total = 0, ng_total = 0;
        auto t0 = std::chrono::steady_clock::now();

        // 相机调参用: 记录上次已下发值（初始即界面默认值，避免启动重复下发）
        int last_expo = Config::CAMERA_EXPOSURE;
        int last_gain = Config::CAMERA_GAIN;

        std::cout << "系统就绪（PLC 触发 / 界面手动拍照 / 回车后门）\n" << std::endl;

        while (running) {
            // 保持 UI 响应（事件泵）
            app.processEvents();

            // 退出检查放在循环最前面：否则空闲(等 PLC 触发)时会 continue 跳到底部检查之前
            if (win.exitRequested()) { running = false; break; }

            // ---- 相机调参: 界面值变了就下发（工程师调参用） ----
            int expo = win.exposureUs(), gain = win.gainDb();
            if (expo != last_expo) { cam.setExposureTime((float)expo); last_expo = expo; }
            if (gain != last_gain) { cam.setGain((float)gain);         last_gain = gain; }

            // ---- 触发源: ①界面手动拍照 ②PLC ③回车后门 ----
            bool triggered = false;

            if (win.takeManualTrigger()) {
                triggered = true;
                std::cout << "[UI] 手动拍照" << std::endl;
            }

            if (!triggered && plc.waitTrigger(50)) {
                triggered = true;
                std::cout << "[PLC] 收到触发信号 → 拍照" << std::endl;
            }

            if (!triggered) {
                struct pollfd pfd;
                pfd.fd     = STDIN_FILENO;
                pfd.events = POLLIN;
                if (poll(&pfd, 1, 0) > 0) {
                    std::string line;
                    std::getline(std::cin, line);
                    triggered = true;
                    std::cout << "[后门] 回车触发" << std::endl;
                }
            }

            if (!triggered) {
                // 空闲: 刷新 PLC 状态 / GPU 温度
                win.setPlcConnected(plc.isConnected());
                win.setGpuTemp(getGPUTemp());
                continue;
            }

            // 新一板开始：先清空上一板结果（HR1←0），避免 PLC 在本周期读到旧状态
            plc.resetResult();

            PerfTimer pt;

            // 软触发相机拍照：先记当前帧序号，触发后等本次触发的新帧
            uint64_t since = cam.frameCount();
            cam.softwareTrigger();

            cv::Mat frame = cam.readNewest(since, 500);
            if (frame.empty()) {
                std::cerr << "[Camera] 触发后未获取到图像" << std::endl;
                continue;
            }

            pt.tick("拍照");

            // 存图总开关：界面「存图开关」需密码开启（默认关，防硬盘写满）；
            // 开启后 原始图/OK/NG 结果图 按同一个比例抽样（默认 50% = 每 2 张存 1 张）
            bool save_this = false;
            if (win.saveEnabled()) {
                int pct = win.rawSaveRatioPct();
                static uint64_t shot = 0;
                if (pct > 0 && (++shot % (100 / pct)) == 0)
                    save_this = true;
            }

            if (save_this && saveAllowed()) {
                g_savedBytes += fileSizeBytes(saveOriginalImage(frame));
            }

            // CLAHE 增强（默认关闭，开启时在此对 frame 做增强）
            cv::Mat img = frame;

            pt.tick("增强");

            // 推理
            auto res = infer.detect(img);

            pt.tick("推理");

            // 后处理 + 画框
            cv::Size sz(img.cols, img.rows);
            auto defects = post.process(res, img, sz);

            // 木板长宽测量
            cv::Mat K = makeK(Config::FX, Config::FY, Config::CX, Config::CY);
            auto measure = measureBoard(img, K, Config::DISTANCE_MM);
            if (measure.valid) {
                cv::Point2f rc[4];
                measure.rrect.points(rc);
                for (int i = 0; i < 4; ++i)
                    cv::line(img, rc[i], rc[(i + 1) % 4], cv::Scalar(255, 0, 0), 2);
            }

            pt.tick("测量");

            // NG 判定：用界面输入的工人阈值（jieba/dongba 数量、dongban/quebian 面积、组合判定）
            post.setJiebaMaxCount(win.jiebaMaxCount());
            post.setDongbaMaxCount(win.dongbaMaxCount());
            post.setDongbanAreaRatio(win.dongbanAreaPct() / 100.0f);
            post.setQuebianAreaRatio(win.quebianAreaPct() / 100.0f);
            post.setJiebaDongbaMaxCount(win.jiebaDongbaMaxCount());
            post.setDongbanQuebianAreaRatio(win.dongbanQuebianAreaPct() / 100.0f);
            std::string ng_reason;
            bool is_ng = post.isNG(defects, sz, ng_reason);
            if (is_ng) ng_total++;
            total++;

            // 发送结果给 PLC
            if (is_ng) {
                plc.sendNG();
                std::cout << "[PLC] → NG" << std::endl;
            } else {
                plc.sendOK();
                std::cout << "[PLC] → OK" << std::endl;
            }

            pt.dump();

            // 结果图（OK/NG 统一）按同一个比例抽样保存（超 1GB 保护闸）
            if (save_this && Config::SAVE_IMAGES && saveAllowed()) {
                g_savedBytes += fileSizeBytes(post.save(img, is_ng, Config::OUTPUT_DIR));
            }
            win.setSaveBlocked(g_saveBlocked);   // 超限时界面提示「存图已停」

            // ---- 刷新界面 ----
            win.setImage(img);
            win.setResult(is_ng, QString::fromStdString(ng_reason));
            win.setStats(total, ng_total);
            win.setGpuTemp(getGPUTemp());
            if (measure.valid) win.setMeasure(measure.long_mm, measure.short_mm);
            win.setCycleMs(pt.elapsed());

            // 统计
            fps.add(pt.elapsed());

            if (total % 50 == 0)
                std::cout << "FPS:" << std::fixed << std::setprecision(1) << fps.val()
                          << " | 检测:" << total << " | NG:" << ng_total << std::endl;

        }

        plc.stop();
        cam.stop();
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
