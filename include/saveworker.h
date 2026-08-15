#pragma once

/**
 * 异步存图线程 — 主线程只推任务，JPEG 编码 + 写盘全在后台线程
 *
 * 为什么：存图（编码 + 写盘）在主线程会阻塞检测、拖慢下一板。
 * 这里用「有上限队列 + 独立线程」把存图挪到后台，主循环零阻塞。
 *
 * 策略：
 *   - 队列满 → 丢弃本次存图（存图是抽样的，丢一张无所谓），主线程永不等待
 *   - 累计存图超 1GB → 停存（防硬盘写满），blocked() 供界面显示提示
 *   - 原始图存 output/raw/RAW_*.jpg（质量 80）；结果图存 output/OK_|NG_*.jpg（质量 70）
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

#include <opencv2/opencv.hpp>

#include "config.h"

class SaveWorker {
public:
    explicit SaveWorker(size_t maxQueue = 4) : _maxQueue(maxQueue) {
        _th = std::thread([this] { run(); });
    }

    ~SaveWorker() { stop(); }

    /** 主线程调用：把存图任务丢进队列，不阻塞。
     *  已停存 / 队列满 / 空图 → 返回 false（丢弃）。img 会被深拷贝。 */
    bool push(const cv::Mat& img, bool is_ng, bool raw) {
        if (_blocked.load()) return false;
        if (img.empty()) return false;

        cv::Mat clone;                 // 深拷贝：主线程后面会继续原地改图（画框/测量）
        img.copyTo(clone);

        std::lock_guard<std::mutex> lk(_mtx);
        if (_stop || _queue.size() >= _maxQueue) return false;
        _queue.push({std::move(clone), is_ng, raw});
        _cv.notify_one();
        return true;
    }

    /** 超 1GB 停存状态（主线程每板轮询刷新界面提示） */
    bool blocked() const { return _blocked.load(); }

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
        cv::Mat img;
        bool    is_ng;
        bool    raw;
    };

    static constexpr uint64_t SAVE_CAP_BYTES    = 1024ULL * 1024ULL * 1024ULL;  // 1GB
    static constexpr int      RAW_JPG_QUALITY   = 80;   // 原始图留存诊断
    static constexpr int      RESULT_JPG_QUALITY = 70;  // 结果图带画框，够用

    void run() {
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lk(_mtx);
                _cv.wait(lk, [this] { return _stop || !_queue.empty(); });
                if (_stop && _queue.empty()) break;
                job = std::move(_queue.front());
                _queue.pop();
            }
            save(job);
        }
    }

    // 只在后台线程执行：编码 + 写盘 + 累计字节
    void save(const Job& job) {
        if (job.img.empty()) return;

        std::string dir = job.raw
            ? std::string(Config::OUTPUT_DIR) + "raw/"
            : Config::OUTPUT_DIR;
        mkdir(dir.c_str(), 0755);

        std::ostringstream ss;
        if (job.raw)      ss << dir << "RAW_";
        else              ss << dir << (job.is_ng ? "NG_" : "OK_");
        ss << timestamp() << ".jpg";

        std::vector<int> jpg{cv::IMWRITE_JPEG_QUALITY,
                             job.raw ? RAW_JPG_QUALITY : RESULT_JPG_QUALITY};
        if (!cv::imwrite(ss.str(), job.img, jpg)) return;
        std::cout << "[Save] " << (job.raw ? "原始图" : "结果图")
                  << " → " << ss.str() << std::endl;

        // 累计字节，超 1GB 停存（_bytes 只有本线程写，不用锁）
        struct stat st;
        if (stat(ss.str().c_str(), &st) == 0) _bytes += (uint64_t)st.st_size;
        if (_bytes >= SAVE_CAP_BYTES) {
            _blocked = true;
            std::cerr << "[SaveGuard] 累计存图超 1GB，停止存图（防硬盘写满）" << std::endl;
        }
    }

    static std::string timestamp() {
        auto now = std::chrono::system_clock::now();
        auto t   = std::chrono::system_clock::to_time_t(now);
        auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                       now.time_since_epoch()) % 1000;
        std::ostringstream ss;
        ss << std::put_time(std::localtime(&t), "%Y%m%d_%H%M%S_")
           << std::setfill('0') << std::setw(3) << ms.count();
        return ss.str();
    }

    size_t                  _maxQueue = 4;
    std::thread             _th;
    std::mutex              _mtx;
    std::condition_variable _cv;
    std::queue<Job>         _queue;
    std::atomic<bool>       _blocked{false};
    bool                    _stop = false;   // 只在 mutex 保护下读写
    uint64_t                _bytes = 0;      // 只在 save() 累加
};
