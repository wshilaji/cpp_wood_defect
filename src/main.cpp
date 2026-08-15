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
#include "saveworker.h"
#include "cameraguard.h"
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
// GPU 温度（低优先级：只喂状态栏显示，不参与检测）
// 只在主循环【空闲】分支刷新（refreshGpuTemp 仅在空闲处调用），
// 触发拍照的检测路径零温度 I/O；一次读缓存 60s。
// ============================================================
static float g_gpuTemp = -1;

static void refreshGpuTemp() {
    static auto last = std::chrono::steady_clock::time_point{};
    auto now = std::chrono::steady_clock::now();
    if (g_gpuTemp < 0 || std::chrono::duration<double>(now - last).count() > 60.0) {
        std::ifstream f("/sys/devices/virtual/thermal/thermal_zone1/temp");
        if (f.is_open()) {
            int raw;
            f >> raw;
            g_gpuTemp = raw / 1000.0f;
        }
        last = now;
    }
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

    // 相机连接函数（启动 + 掉线重连共用）：失败不退出，由 CameraGuard 后台限频重连。
    // systemd 自动重启场景下启动即 return 会无限循环重启，所以这里只标记、不退出。
    auto connectCamera = [&]() -> bool {
        cam.stop();   // 清残留状态保证重连干净（首次调用无操作）
        if (!cam.connectByIP(Config::CAMERA_IP)) {
            std::cerr << "[Camera] 连接失败" << std::endl;
            return false;
        }
        // 用界面当前曝光/增益启动（工人调过的参数掉线重连后不丢）
        if (!cam.start(Config::CAMERA_WIDTH, Config::CAMERA_HEIGHT,
                       (float)win.exposureUs(), (float)win.gainDb(),
                       Config::CAMERA_TRIGGER)) {
            std::cerr << "[Camera] 取流启动失败" << std::endl;
            cam.stop();
            return false;
        }
        return true;
    };

    try {
        if (!connectCamera()) {
            std::cerr << "[Camera] 启动连接失败，进入后台重连模式（主循环持续重试）" << std::endl;
        }
        win.setCamRunning(cam.isRunning());

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
        if (!cam.isRunning()) plc.sendReady(false);   // 相机未就绪 → HR2=0，PLC 知道机器故障

        // ---- 相机守护：掉线自动重连 + 连续空帧判故障通知 PLC ----
        CameraGuard camGuard(connectCamera, [&](bool ready) {
            plc.sendReady(ready);          // 就绪 → HR2=1；故障 → HR2=0
            win.setCamFault(!ready);       // 故障 → 红灯；恢复 → 清红灯
            win.setCamRunning(ready);      // 就绪 → 绿
            if (ready) std::cout << "[Camera] 重连成功" << std::endl;
            else       std::cerr << "[Camera] 连续空帧判定故障 → 通知 PLC（HR2=0）" << std::endl;
        }, cam.isRunning());

        // ---- 异步存图线程（编码+写盘在后台，主循环零阻塞） ----
        SaveWorker saver;

        // ---- 预热: 触发一次填满相机管线，避免首次拍照丢帧（相机未就绪则跳过） ----
        if (cam.isRunning()) {
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

            // 相机健康管理：未连接则限频(1s)后台重连；UI 相机灯实时同步
            camGuard.poll();
            win.setCamRunning(cam.isRunning());

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
                // 空闲: 刷新 PLC 状态 / GPU 温度（温度只在空闲读，不占检测路径）
                win.setPlcConnected(plc.isConnected());
                refreshGpuTemp();
                win.setGpuTemp(g_gpuTemp);
                continue;
            }

            // 相机未就绪：本次触发不拍照（触发已消费），PLC 那边看 HR2=0 知道机器故障
            if (!camGuard.running()) {
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
                if (camGuard.onMiss()) cam.stop();   // 连续 3 次空帧判故障：停相机，下轮 poll 自动重连
                continue;
            }
            camGuard.onFrame();   // 拿到帧，健康

            pt.tick("拍照");

            // 存图总开关：界面「开发者模式」需密码开启（默认关，防硬盘写满）；
            // 开启后 原始图 / 结果图 各按自己的比例独立抽样（每 100/pct 张存 1 张）
            bool save_raw = false, save_res = false;
            if (win.saveEnabled()) {
                int rpct = win.rawSaveRatioPct();
                static uint64_t raw_shot = 0;
                if (rpct > 0 && (++raw_shot % (100 / rpct)) == 0) save_raw = true;

                int spct = win.resultSaveRatioPct();
                static uint64_t res_shot = 0;
                if (spct > 0 && (++res_shot % (100 / spct)) == 0) save_res = true;
            }

            // 原始图丢给后台线程存（深拷贝；队列满/已停存则自动丢弃）
            if (save_raw && Config::SAVE_IMAGES) saver.push(frame, false, true);

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
            post.setMinLengthMm(win.minLengthMm());
            post.setMinWidthMm(win.minWidthMm());
            std::string ng_reason;
            // 尺寸判定用测量出的长/宽（未测到传 0，不判尺寸 NG）
            float len_mm = measure.valid ? measure.long_mm : 0.0f;
            float wid_mm = measure.valid ? measure.short_mm : 0.0f;
            bool is_ng = post.isNG(defects, sz, len_mm, wid_mm, ng_reason);
            if (is_ng) ng_total++;
            total++;

            // 发送结果给 PLC（每帧零日志，结果只走 Modbus 不发控制台）
            if (is_ng) plc.sendNG();
            else       plc.sendOK();

            pt.dump();

            // 结果图（OK/NG 统一）丢给后台线程存（超 1GB 保护闸在 worker 内）
            if (save_res && Config::SAVE_IMAGES) saver.push(img, is_ng, false);
            win.setSaveBlocked(saver.blocked());   // 超限时界面提示「存图已停」

            // ---- 刷新界面 ----
            win.setImage(img);
            win.setResult(is_ng, QString::fromStdString(ng_reason));
            win.setStats(total, ng_total);
            win.setGpuTemp(g_gpuTemp);   // 只显示空闲时刷新的缓存值，检测路径零温度 I/O
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
        saver.stop();   // 等后台把排队中的存图写完再退出
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
