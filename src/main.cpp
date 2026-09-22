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
#include <sstream>
#include <string>
#include <cctype>          // std::tolower：thermal zone 的 type 大小写各版 L4T 不统一
#include <sys/statvfs.h>

#include <QApplication>
#include <QString>

#include "config.h"
#include "logger.h"
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

// 收到 SIGINT/SIGTERM 置位。退出后的冷却等待(见 EXIT_RESTART_DELAY_SEC)靠它打断,
// 否则 systemctl stop/restart 要干等到冷却结束, 长到会被 systemd 判超时 SIGKILL。
static std::atomic<bool> g_sigStop{false};

// ============================================================
// 全局指针 + 崩溃清理
// ============================================================
static HikvisionCamera* g_cam   = nullptr;
static PlcLink*         g_plc   = nullptr;

static void cleanup_all() {
    if (g_cam && g_cam->isRunning()) {
        LOGE << "[CrashGuard] 强制释放相机...";
        g_cam->stop();
    }
    if (g_plc) {
        g_plc->stop();
    }
}

static void on_terminate() {
    LOGE << "[CrashGuard] std::terminate 触发";
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
        LOGE << "[CrashGuard] " << name << " 收到，正在退出...";
        running = false;
        g_sigStop = true;   // 打断退出后的冷却等待
        cleanup_all();
        return;
    }

    LOGE << "[CrashGuard] " << name << " 异常信号，尝试清理...";
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
// 系统状态：GPU/CPU 温度、内存占用、硬盘占用（低优先级：只喂状态栏显示，不参与检测）
// 只在主循环【空闲】分支刷新（refreshSysStats 仅在空闲处调用），
// 触发拍照的检测路径零 I/O（只读上一次的缓存值）；一次读缓存 60s。
// ============================================================
static float g_gpuTemp = -1;
static float g_cpuTemp = -1;
static float g_memPct  = -1;
static float g_diskPct = -1;

// 按 thermal zone 的 type 找传感器路径，不写死编号：
// thermal_zoneN 的编号跟板子型号/内核版本有关，写死 zone1 换台机器可能读到别的传感器。
// 找不到返回空串，调用方显示 --。
//
// 匹配必须【不区分大小写】。各版 L4T 的 type 大小写不一样：
//   老 Nano(4GB, 早期 L4T)  type 是大写 "CPU-therm" / "GPU-therm"
//   Orin  (8GB, JetPack5/6) type 是全小写 "cpu-therm" / "gpu-therm"
// 只按大写找的话在 Orin 上全部落空，温度恒显示 "--"。
// 这个坑是 2026-09-22 现场暴露的：改成按 type 找之后 CPU/GPU 温度都不显示了，
// 而老代码写死读 /sys/devices/virtual/thermal/thermal_zone1/temp、压根不看 type，
// 所以从没暴露过 —— 换句话说这问题是我改出来的，不是老代码本来就有的。
static std::string toLowerAscii(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

static std::string findThermalZone(const char* keyword) {
    const std::string key = toLowerAscii(keyword);
    for (int i = 0; i < 16; ++i) {
        std::string base = "/sys/class/thermal/thermal_zone" + std::to_string(i);
        std::ifstream tf(base + "/type");
        if (!tf.is_open()) continue;              // 编号不连续，跳过
        std::string type;
        std::getline(tf, type);
        if (toLowerAscii(type).find(key) != std::string::npos) return base + "/temp";
    }
    return std::string();
}

// 内核给的温度是毫摄氏度
static float readTempC(const std::string& path) {
    if (path.empty()) return -1;
    std::ifstream f(path);
    if (!f.is_open()) return -1;
    int milli = 0;
    f >> milli;
    return milli / 1000.0f;
}

// 内存占用率 %。用 MemAvailable 而不是 MemFree：MemFree 不含可回收的磁盘缓存，
// 会虚高（free 命令的 available 也是这个口径）。
static float readMemUsedPct() {
    std::ifstream f("/proc/meminfo");
    if (!f.is_open()) return -1;
    long long total = 0, avail = 0;
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream is(line);
        std::string key;
        long long kb = 0;
        if (!(is >> key >> kb)) continue;
        if      (key == "MemTotal:")     total = kb;
        else if (key == "MemAvailable:") { avail = kb; break; }   // 排在 MemTotal 之后
    }
    if (total <= 0) return -1;
    return 100.0f * (float)(total - avail) / (float)total;
}

// 硬盘占用率 %：量存图目录所在的那块盘（图都写这儿，它满了才是真问题）。
// 口径跟 df 一致：已用 = (blocks - bfree) / blocks。
static float readDiskUsedPct(const char* path) {
    struct statvfs vfs;
    if (statvfs(path, &vfs) != 0) return -1;
    const unsigned long long total = (unsigned long long)vfs.f_blocks * vfs.f_frsize;
    if (total == 0) return -1;
    const unsigned long long used =
        ((unsigned long long)vfs.f_blocks - (unsigned long long)vfs.f_bfree) * vfs.f_frsize;
    return 100.0f * (float)used / (float)total;
}

static void refreshSysStats() {
    static auto last   = std::chrono::steady_clock::time_point{};
    static bool inited = false;
    auto now = std::chrono::steady_clock::now();
    // 用 inited 而不是拿 g_gpuTemp 判空：找不到传感器时 g_gpuTemp 恒为 -1，
    // 那样每次空闲循环都会重读一遍 /proc/meminfo + statvfs。
    if (inited && std::chrono::duration<double>(now - last).count() <= 60.0) return;

    static std::string gpu_path, cpu_path;   // 只找一次；没找到下次再试
    if (gpu_path.empty()) gpu_path = findThermalZone("GPU");
    if (cpu_path.empty()) cpu_path = findThermalZone("CPU");

    g_gpuTemp = readTempC(gpu_path);
    g_cpuTemp = readTempC(cpu_path);
    g_memPct  = readMemUsedPct();
    g_diskPct = readDiskUsedPct(Config::OUTPUT_DIR);

    inited = true;
    last   = now;
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
            LOGE << "[Camera] 连接失败";
            return false;
        }
        // 用界面当前曝光/增益启动（工人调过的参数掉线重连后不丢）
        if (!cam.start(Config::CAMERA_WIDTH, Config::CAMERA_HEIGHT,
                       (float)win.exposureUs(), (float)win.gainDb(),
                       Config::CAMERA_TRIGGER)) {
            LOGE << "[Camera] 取流启动失败";
            cam.stop();
            return false;
        }
        return true;
    };

    try {
        if (!connectCamera()) {
            LOGE << "[Camera] 启动连接失败，进入后台重连模式（主循环持续重试）";
        }
        win.setCamRunning(cam.isRunning());

        // ---- 推理引擎 ----
        InferEngine infer;
        if (!infer.load(Config::ENGINE_PATH)) {
            LOGE << "引擎加载失败";
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
            LOGE << "PLC TCP Server 启动失败";
            cam.stop();
            return -3;
        }
        if (!cam.isRunning()) plc.sendReady(false);   // 相机未就绪 → HR2=0，PLC 知道机器故障

        // ---- 相机守护：掉线自动重连 + 连续空帧判故障通知 PLC ----
        CameraGuard camGuard(connectCamera, [&](bool ready) {
            plc.sendReady(ready);          // 就绪 → HR2=1；故障 → HR2=0
            win.setCamFault(!ready);       // 故障 → 红灯；恢复 → 清红灯
            win.setCamRunning(ready);      // 就绪 → 绿
            if (ready) LOGI << "[Camera] 重连成功";
            else       LOGE << "[Camera] 连续空帧判定故障 → 通知 PLC（HR2=0）";
        }, cam.isRunning());

        // ---- 异步存图线程（编码+写盘在后台，主循环零阻塞） ----
        SaveWorker saver;

        // ---- 预热: 触发一次填满相机管线，避免首次拍照丢帧（相机未就绪则跳过） ----
        if (cam.isRunning()) {
            uint64_t since = cam.frameCount();
            cam.softwareTrigger();
            cam.readNewest(since, 500);   // 等预热帧到并丢弃
        }

        // ---- 主循环（PLC / 手动拍照 触发） ----
        FPS fps;
        uint64_t total = 0, ng_total = 0;
        auto t0 = std::chrono::steady_clock::now();

        // 相机调参用: 记录上次已下发值（初始即界面默认值，避免启动重复下发）
        int last_expo = Config::CAMERA_EXPOSURE;
        int last_gain = Config::CAMERA_GAIN;

        LOGI << "系统就绪（PLC 触发 / 界面手动拍照）";

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

            // ---- 触发源: ①界面手动拍照 ②PLC ----
            // 板来源(PLC/手动)收进 PlcLink: PLC 板才写结果+握手, 手动板纯 debug 不碰 PLC。
            // 主循环无需关心 from_plc, 只管「开板 → 拍照推理 → reportResult」
            auto src = plc.beginBoard(win.takeManualTrigger(), 50);

            if (src == BoardSource::None) {
                // 空闲: 刷新 PLC 状态 / 系统状态（温度·内存·硬盘只在空闲读，不占检测路径）
                win.setPlcConnected(plc.isConnected());
                refreshSysStats();
                win.setGpuTemp(g_gpuTemp);
                win.setCpuTemp(g_cpuTemp);
                win.setMemoryPct(g_memPct);
                win.setDiskPct(g_diskPct);
                continue;
            }
            if (src == BoardSource::Manual)
                LOGI << "[UI] 手动拍照";

            // 相机未就绪：本次触发不拍照（触发已消费），PLC 那边看 HR2=0 知道机器故障
            if (!camGuard.running()) {
                continue;
            }

            // 板开始清 HR1 残留已由 beginBoard 在 PLC 板内部处理，主循环不管
            PerfTimer pt;

            // 软触发相机拍照：先记当前帧序号，触发后等本次触发的新帧
            uint64_t since = cam.frameCount();
            cam.softwareTrigger();

            cv::Mat frame = cam.readNewest(since, 500);
            if (frame.empty()) {
                LOGE << "[Camera] 触发后未获取到图像";
                if (camGuard.onMiss()) cam.stop();   // 连续 3 次空帧判故障：停相机，下轮 poll 自动重连
                continue;
            }
            camGuard.onFrame();   // 拿到帧，健康

            // 本板存图 ID：原始图（下面先推）和结果图（判完 NG 再推）共用这一个，
            // 两张图命名成 <id>_RAW.jpg / <id>_NG|OK.jpg，后缀一致才能一一对应。
            // 在这里生成（拿到帧时）而不是写盘时，ID 反映的才是拍照时刻。
            const std::string board_id = SaveWorker::makeBoardId();

            pt.tick("拍照");

            // OK 板的抽样比例（只有开发者模式才看这个）。NG 板一律全存，不吃这个开关，
            // 见下面判完 NG 之后的推送处。
            const int ok_raw_pct = win.saveEnabled() ? win.rawSaveRatioPct()    : 0;
            const int ok_res_pct = win.saveEnabled() ? win.resultSaveRatioPct() : 0;

            // 必须 clone：原始图要"没画过框"的干净帧，而结果图得等判完 NG 才存。
            // 原先开发者模式是抢在画框【之前】推原始图的（老代码那行在 cv::Mat img = frame
            // 之前），当时"存不存"只看比例、跟 NG 无关，所以抢得到；现在"存不存"取决于
            // is_ng，而 is_ng 在画框之后才有 —— 提前推这条路堵死了，只能先把干净帧复制出来。
            // cv::Mat img = frame 是浅拷贝（共享像素缓冲），画框会连 frame 一起画花，所以不能省。
            // 代价：2448×2048×3 ≈ 15MB/板；循环退出即释放，峰值只多一帧，相对 8G 可忽略。
            // CLAHE 增强（默认关闭，开启时在此对 img 做增强）
            cv::Mat img = frame.clone();

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
            post.setHeibaMaxCount(win.heibaMaxCount());
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

            // 报本板结果: PLC 板写 HR1+HR3(握手), 手动 debug 板自动忽略, PLC 状态保持干净
            plc.reportResult(!is_ng);

            pt.dump();

            // ---- 存图 ----
            //   NG 板 → 原始图(frame，干净) + 结果图(img，带框)，【无条件全存】，
            //           不吃开发者模式开关 —— 留档不该依赖谁记得去开那个开关。
            //   OK 板 → 默认不存；开发者模式开着时按各自比例抽样（查漏检用）。
            // 写不写由 worker 说了算（磁盘空间/目录总量两道闸），它拒收就丢弃，主线程不等。
            if (Config::SAVE_IMAGES) {
                if (is_ng) {
                    saver.push(frame, true, true,  board_id);   // 原始图
                    saver.push(img,   true, false, board_id);   // 结果图
                } else {
                    static uint64_t ok_raw_shot = 0, ok_res_shot = 0;
                    if (ok_raw_pct > 0 && (++ok_raw_shot % (100 / ok_raw_pct)) == 0)
                        saver.push(frame, false, true,  board_id);
                    if (ok_res_pct > 0 && (++ok_res_shot % (100 / ok_res_pct)) == 0)
                        saver.push(img,   false, false, board_id);
                }
            }
            // 停写提示带上原因（磁盘不足 / 目录超 60G），现场看提示就知道该清哪儿
            win.setSaveBlocked(saver.blocked(), QString::fromUtf8(saver.blockedReason()));

            // ---- 刷新界面 ----
            win.setImage(img);
            win.setResult(is_ng, QString::fromStdString(ng_reason));
            win.setStats(total, ng_total);
            // 系统状态只显示空闲时刷新的缓存值，检测路径零 I/O
            win.setGpuTemp(g_gpuTemp);
            win.setCpuTemp(g_cpuTemp);
            win.setMemoryPct(g_memPct);
            win.setDiskPct(g_diskPct);
            if (measure.valid) win.setMeasure(measure.long_mm, measure.short_mm);
            win.setCycleMs(pt.elapsed());

            // 统计
            fps.add(pt.elapsed());

            if (total % 50 == 0)
                LOGI << "FPS:" << std::fixed << std::setprecision(1) << fps.val()
                     << " | 检测:" << total << " | NG:" << ng_total;

        }

        // 先藏窗口收尾: 退出是为了腾桌面(开 RustDesk / 远程协助 / 改网络), 藏窗口要
        // 立刻, 不能等 saver.stop() 把排队存图写完(可能好几秒), 那几秒全屏置顶还盖着。
        win.hide();
        app.processEvents();

        plc.stop();
        cam.stop();
        saver.stop();   // 等后台把排队中的存图写完再退出
        // 下面冷却等待期间还可能收到信号, cleanup_all() 会经 g_plc/g_cam 去停设备。
        // plc 随本 try 作用域析构, 之后信号再进来就是解引用已析构对象 → 必须清空指针。
        // (cam 在外层作用域不会析构, 但句柄已关, 一并清掉省得误判为"还在跑")
        g_plc = nullptr;
        g_cam = nullptr;
        auto dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        LOGI << "停止 | 运行:" << (int)dt << "s | 检测:" << total
             << " | NG:" << ng_total;

    } catch (const std::exception& e) {
        LOGE << "异常: " << e.what();
        if (g_plc) g_plc->stop();
        cam.stop();
        return -5;
    } catch (...) {
        LOGE << "未知异常";
        if (g_plc) g_plc->stop();
        cam.stop();
        return -6;
    }

    // ============================================================
    // 界面「退出」后的冷却等待
    // ============================================================
    // 走到这里说明是界面点「退出」的正常退出(异常/崩溃在上面已 return, 不走这段)。
    // 设备已全停: PlcLink(线程 join)/SaveWorker(线程 join)/推理引擎 随 try 作用域析构,
    // 引擎显存随之释放; 相机 stop() 里 closeDevice() 已 DestroyHandle 释放句柄。
    // 所以进程现在只剩 Qt 窗口对象在空转, 不占相机、不占 GPU、不占后台线程。
    //
    // 为什么不能直接 return: systemd 是 Restart=always, 进程一结束就按 RestartSec(3s)
    // 把界面拉回来。而要开 RustDesk 让远程协助连进来、或改网络设置, 几秒钟根本不够,
    // 全屏置顶窗口一回来桌面又被盖住。所以这里不结束进程, 原地等 EXIT_RESTART_DELAY_SEC:
    // systemd Type=simple 只看主进程死没死, 进程活着就不会重启 → 这段时间界面不会回来。
    // 等够了进程正常退出, systemd 才按 RestartSec=3 拉起, 总延时≈配置值。
    // 崩溃路径不受影响: 段错误/OOM 直接死, 产线恢复仍是 3 秒。
    LOGI << "[Exit] 界面已隐藏, " << Config::EXIT_RESTART_DELAY_SEC
         << " 秒后再由 systemd 自动拉起"
         << " (想提前恢复: sudo systemctl restart wood-defect-detector)";

    // 逐秒等而不是一次 sleep 到底: 收到 SIGTERM 要能立刻走, 否则 `systemctl stop/restart`
    // 会卡到 systemd 的 TimeoutStopSec(默认 90s) 超时被 SIGKILL, 白等一场。
    for (int left = Config::EXIT_RESTART_DELAY_SEC; left > 0; --left) {
        if (g_sigStop) {
            LOGI << "[Exit] 收到停止信号, 结束等待立即退出";
            break;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    LOGI << "[Exit] 等待结束, 进程退出";
    return 0;
}
