#pragma once

#include "logger.h"

/**
 * 异步存图线程 — 主线程只推任务，JPEG 编码 + 写盘全在后台线程
 *
 * 为什么：存图（编码 + 写盘）在主线程会阻塞检测、拖慢下一板。
 * 这里用「有上限队列 + 独立线程」把存图挪到后台，主循环零阻塞。
 *
 * 策略：
 *   - 队列满 → 丢弃本次存图（存图是抽样的，丢一张无所谓），主线程永不等待
 *   - 原始图存 output/raw/<id>_RAW.jpg（质量 80）；结果图存 output/result/<id>_NG|OK.jpg（质量 70）
 *     <id> 由调用方一板生成一个、两张图共用（见 makeBoardId），所以同一块板的
 *     原始图/结果图后缀完全一致，合并两个目录就能一一对应；ID 放最前面，
 *     按名字排序时两张图天然相邻成对（旧格式 NG_/RAW_ 前缀不同，排序是两堆分开的）。
 *
 * 防写满盘 —— 双保险里的【C++ 这层】，另一层是 cleanup_images.sh + systemd timer：
 *   ① 磁盘剩余空间 < SAVE_MIN_FREE_BYTES → 停写。
 *      防的不只是自己：日志、别的程序、别的目录都可能把盘吃掉。这条才是
 *      "别把盘写满" 的真正保证，跟留存策略无关。
 *   ② raw/ + result/ 实测总量 ≥ SAVE_CAP_BYTES(60G) → 停写。
 *      这是 60G 留存策略的兜底：脚本那层负责删旧的（删到 54G 水位），脚本要是
 *      没跑起来／挂了，这里至少不会无限涨。
 *   两条都是【定期复查、自动恢复】的，不是一次性锁死 —— 脚本把空间腾出来之后
 *   不用重启程序就能继续存。（上一版是一旦超 1GB 就永久 _blocked、而且 _bytes
 *   只活在内存里重启就归零，既停不回来也算不准，那个设计是错的，已换掉。）
 *
 * 主线程 push() 只读一个 atomic，不做任何 I/O / 不 stat / 不扫目录 —— 检测路径零开销。
 */

#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <string>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <iostream>
#include <cstdint>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <dirent.h>

#include <opencv2/opencv.hpp>

#include "config.h"

class SaveWorker {
public:
    explicit SaveWorker(size_t maxQueue = 4) : _maxQueue(maxQueue) {
        // 存图目录一次性建好(mkdir 只建一层, 父目录先建), save() 里不再检查
        ::mkdir("./output", 0755);
        ::mkdir("./output/raw", 0755);
        ::mkdir("./output/result", 0755);
        _th = std::thread([this] { run(); });
    }

    ~SaveWorker() { stop(); }

    /** 主线程调用：把存图任务丢进队列，不阻塞。
     *  已停写 / 队列满 / 空图 → 返回 false（丢弃）。img 会被深拷贝。
     *  @param id  本板存图 ID。同一块板的原始图和结果图必须传同一个，
     *             否则两张图名字对不上，回溯时配不起来。 */
    bool push(const cv::Mat& img, bool is_ng, bool raw, const std::string& id) {
        if (blocked()) return false;   // 只读 atomic，不做 I/O
        if (img.empty()) return false;

        cv::Mat clone;                 // 深拷贝：主线程后面会继续原地改图（画框/测量）
        img.copyTo(clone);

        std::lock_guard<std::mutex> lk(_mtx);
        if (_stop || _queue.size() >= _maxQueue) return false;
        _queue.push({std::move(clone), is_ng, raw, id});
        _cv.notify_one();
        return true;
    }

    /** 生成一块板的存图 ID，形如 20260922_133031_314。
     *  调用方在【拿到帧时】生成一次，本板的原始图和结果图都传它：
     *    - 两张图后缀完全一致 → 一一对应是确定的，不靠时间戳接近去猜
     *    - ID 反映的是拍照时刻而不是写盘时刻，队列积压时不会漂移
     *  毫秒精度足够唯一：主循环一板 ~200ms，不可能撞上。 */
    static std::string makeBoardId() {
        auto now = std::chrono::system_clock::now();
        auto t   = std::chrono::system_clock::to_time_t(now);
        auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                       now.time_since_epoch()) % 1000;
        std::ostringstream ss;
        ss << std::put_time(std::localtime(&t), "%Y%m%d_%H%M%S_")
           << std::setfill('0') << std::setw(3) << ms.count();
        return ss.str();
    }

    /** 是否处于停写状态（主线程每板轮询刷新界面提示） */
    bool blocked() const { return _blockCode.load() != BLK_NONE; }

    /** 停写原因，给界面显示用；没停写返回空串 */
    const char* blockedReason() const {
        // 数字从 SAVE_CAP_BYTES 推出来，不写死：改上限时界面文案不会漏改
        static const std::string dir_full =
            "存图目录超 " + std::to_string(SAVE_CAP_BYTES >> 30) + "G";
        switch (_blockCode.load()) {
            case BLK_LOW_DISK: return "磁盘空间不足";
            case BLK_DIR_FULL: return dir_full.c_str();
            default:           return "";
        }
    }

    /** 停止：把队列里排队的图存完再退出 */
    void stop() {
        {
            std::lock_guard<std::mutex> lk(_mtx);
            if (_stop) return;
            _stop = true;
        }
        _cv.notify_all();
        if (_th.joinable()) _th.join();
    }

private:
    struct Job {
        cv::Mat     img;
        bool        is_ng;
        bool        raw;
        std::string id;      // 本板存图 ID（原始图/结果图同一个）
    };

    enum BlockCode { BLK_NONE = 0, BLK_LOW_DISK = 1, BLK_DIR_FULL = 2 };

    static constexpr uint64_t SAVE_CAP_BYTES      = 60ULL * 1024 * 1024 * 1024;  // 目录总量上限 60G
    static constexpr uint64_t SAVE_MIN_FREE_BYTES = 10ULL * 1024 * 1024 * 1024;  // 磁盘至少留 10G
    static constexpr int      RECHECK_SEC         = 30;  // 上述两条的复查间隔(秒)

    // 清理脚本删到的水位。必须与 cleanup_images.sh 的 CLEANUP_KEEP_GB 一致（改一个就得改另一个）。
    // 取上限的 90%，跟脚本那边的默认值同比例 —— 水位贴着上限的话，脚本刚清完就又被判超限，
    // 来回抖；水位离上限太远（比如加了上限却不抬水位）则等于上限没加，多出来的空间白留。
    static constexpr uint64_t SAVE_WATERMARK_BYTES = SAVE_CAP_BYTES / 10 * 9;   // 54G

    // 重扫目录的门槛。必须落在【水位(54G)】和【上限(60G)】之间，取正中间：
    //   - 定成水位那个数就是踩过的坑：脚本常态把目录维持在水位，估算值永远 ≥ 门槛，
    //     于是每 30 秒全量扫一次目录（几万个文件、几万次 stat），eMMC 上白白消耗寿命。
    //   - 取中间(57G)：脚本刚清完(54G)之后要再写 3G 才重扫一次（NG 率 5% 约 6 天一次）。
    // 也不会因此误判停写 —— 估算涨到 60G 必然先经过 57G，那时已经重扫拿到真值了。
    static constexpr uint64_t RESCAN_AT_BYTES     =
        SAVE_WATERMARK_BYTES + (SAVE_CAP_BYTES - SAVE_WATERMARK_BYTES) / 2;     // 57G
    // 已经停写时的重扫间隔。停写意味着目录一直卡在上限以上，按上面的门槛是
    // 「每次都重扫」，不限制就成了永远每 30 秒一次全量扫。放宽到 2 分钟：
    // 恢复延迟从 30 秒变 2 分钟，而脚本本身也是一小时才跑一次，这点延迟无所谓。
    static constexpr int      RESCAN_BLOCKED_SEC  = 120;
    static constexpr int      RAW_JPG_QUALITY     = 80;  // 原始图留存诊断
    static constexpr int      RESULT_JPG_QUALITY  = 70;  // 结果图带画框，够用

    void run() {
        // 减掉一个间隔 → 进循环立刻复查一次，别等 30 秒才第一次判
        auto last_check = std::chrono::steady_clock::now() - std::chrono::seconds(RECHECK_SEC);

        for (;;) {
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration<double>(now - last_check).count() >= RECHECK_SEC) {
                recheck();
                last_check = now;
            }

            Job  job;
            bool have = false;
            {
                std::unique_lock<std::mutex> lk(_mtx);
                // 1 秒醒一次是给定期复查用的：停写期间队列是空的、没人 notify，
                // 只等 notify 的话复查永远不跑，脚本把空间腾出来了也恢复不了。
                _cv.wait_for(lk, std::chrono::seconds(1),
                             [this] { return _stop || !_queue.empty(); });
                if (_stop && _queue.empty()) break;
                if (!_queue.empty()) {
                    job = std::move(_queue.front());
                    _queue.pop();
                    have = true;
                }
            }

            if (have && !blocked()) save(job);
        }
    }

    // ---- 防写满盘的复查（只在后台线程跑） ----

    void recheck() {
        const int prev = _blockCode.load();
        int now = BLK_NONE;

        struct statvfs vfs;
        if (::statvfs(Config::OUTPUT_DIR, &vfs) == 0) {
            // f_bavail（非特权用户可用）而不是 f_bfree：后者含 root 保留块
            const uint64_t free_bytes = (uint64_t)vfs.f_bavail * (uint64_t)vfs.f_frsize;
            if (free_bytes < SAVE_MIN_FREE_BYTES) now = BLK_LOW_DISK;
        }
        if (now == BLK_NONE && dirBytesEstimate() >= SAVE_CAP_BYTES) now = BLK_DIR_FULL;

        _blockCode = now;

        // 只在状态跳变时打日志。平时 30 秒一次静默通过 —— 打日志本身要写盘，
        // 每 30 秒一条看着不多，但一年就是几十万行，把日志文件本身撑起来很蠢。
        // 但跳变必须留痕：现场「怎么突然不存图了」只能靠这几行定位。
        if (now != prev) {
            if (now == BLK_NONE)
                LOGI << "[Save] 恢复存图（之前停写原因已解除）";
            else if (now == BLK_LOW_DISK)
                LOGE << "[Save] 停写：磁盘可用空间不足 " << (SAVE_MIN_FREE_BYTES >> 30) << "G";
            else
                LOGE << "[Save] 停写：raw+result 总量已达 " << (SAVE_CAP_BYTES >> 30)
                     << "G 上限，等 cleanup_images.sh 清理";
        }
    }

    // raw/ + result/ 的实测总量（两个目录加起来算，跟清理脚本一个口径）
    static uint64_t dirBytes() {
        uint64_t total = 0;
        for (const char* sub : {"raw/", "result/"}) {
            const std::string dir = std::string(Config::OUTPUT_DIR) + sub;
            DIR* d = ::opendir(dir.c_str());
            if (!d) continue;
            while (struct dirent* e = ::readdir(d)) {
                if (e->d_name[0] == '.') continue;
                struct stat st;
                if (::stat((dir + e->d_name).c_str(), &st) == 0 && S_ISREG(st.st_mode))
                    total += (uint64_t)st.st_size;
            }
            ::closedir(d);
        }
        return total;
    }

    // 总量估算：平时用「上次实测 + 之后本进程写入的」，离上限还远就不扫目录。
    // 扫几万个文件是秒级的，不能每 30 秒来一次。
    uint64_t dirBytesEstimate() {
        const auto now = std::chrono::steady_clock::now();
        bool due = false;
        if (blocked()) {
            // 停写期间【不看门槛，只看时间】：停写本身就意味着估算值在门槛以上
            // （要么真超 60G、要么盘快满），门槛那条判断这时恒成立，先判它就等于
            // 每 30 秒全量扫一次目录 —— 这条限频就是白写的（实测踩过：停写后
            // 删掉文件 30 秒就恢复了，而设计上该是 120 秒）。
            // 恢复延迟最多 RESCAN_BLOCKED_SEC，而清理脚本本身也是一小时才跑一次，
            // 这点延迟无所谓；换来停写期间不再拿几万个文件的 stat 去打 eMMC。
            due = !_scanned ||
                  std::chrono::duration<double>(now - _lastScan).count() >= RESCAN_BLOCKED_SEC;
        } else if (!_scanned || _scanBytes + _writtenSinceScan >= RESCAN_AT_BYTES) {
            due = true;
        }
        if (due) {
            _scanBytes = dirBytes();
            _writtenSinceScan = 0;
            _lastScan = now;
            _scanned = true;
        }
        return _scanBytes + _writtenSinceScan;
    }

    // 只在后台线程执行：编码 + 写盘 + 累计字节
    void save(const Job& job) {
        if (job.img.empty()) return;

        // 目录已在构造时建好, 这里不再 mkdir
        std::string dir = job.raw
            ? std::string(Config::OUTPUT_DIR) + "raw/"
            : std::string(Config::OUTPUT_DIR) + "result/";

        // ID 放最前面: 合并 raw/ 与 result/ 后按名字排序，同一块板的两张图相邻。
        std::ostringstream ss;
        ss << dir << job.id
           << (job.raw ? "_RAW" : (job.is_ng ? "_NG" : "_OK")) << ".jpg";

        std::vector<int> jpg{cv::IMWRITE_JPEG_QUALITY,
                             job.raw ? RAW_JPG_QUALITY : RESULT_JPG_QUALITY};
        if (!cv::imwrite(ss.str(), job.img, jpg)) {
            LOGE << "[Save] 写图失败: " << ss.str();   // 之前静默失败, 现在报出来
            return;
        }
        LOGI << "[Save] " << (job.raw ? "原始图" : "结果图")
             << " → " << ss.str();

        // 累计本进程写入量，供 dirBytesEstimate() 用（只有本线程写，不用锁）
        struct stat st;
        if (::stat(ss.str().c_str(), &st) == 0) _writtenSinceScan += (uint64_t)st.st_size;
    }

    size_t                  _maxQueue = 4;
    std::thread             _th;
    std::mutex              _mtx;
    std::condition_variable _cv;
    std::queue<Job>         _queue;
    std::atomic<int>        _blockCode{BLK_NONE};
    bool                    _stop = false;   // 只在 mutex 保护下读写

    // 下面这些只在后台线程读写，不用锁
    bool     _scanned          = false;
    uint64_t _scanBytes        = 0;   // 上次实测的目录总量
    uint64_t _writtenSinceScan = 0;   // 那之后本进程写入的字节
    std::chrono::steady_clock::time_point _lastScan{};   // 上次实测时刻（停写时限频用）
};
