#pragma once

/**
 * 性能计时工具
 *
 * - tick() / elapsed() 始终启用：cycle 时间显示需要（每次只记录一个时间点，纳秒级开销）
 * - dump() 是联调打印：默认关闭（下面 PERF_LOG 被注释），生产环境每帧零打印
 *
 * 用法:
 *   PerfTimer t;
 *   t.tick("拍照");
 *   t.tick("推理");
 *   t.dump();                     // 只在 PERF_LOG 开启时打印各段耗时
 *   win.setCycleMs(t.elapsed());  // 始终可用
 */

// ========== 联调开关：要打印各段耗时就把下面这行打开 ==========
// #define PERF_LOG

#include <chrono>
#include <string>
#include <vector>

#ifdef PERF_LOG
#include <iostream>
#include <iomanip>
#include <sstream>
#endif

class PerfTimer {
public:
    PerfTimer() { _points.emplace_back("start", Clock::now()); }

    /** 记录一个时间点（始终启用，成本：一次 steady_clock::now + push_back） */
    void tick(const std::string& name) {
        _points.emplace_back(name, Clock::now());
    }

    /** 从第一个点到最后一个点的总耗时 (ms)，始终可用（cycle 显示） */
    double elapsed() const {
        if (_points.size() < 2) return 0;
        return msBetween(_points.front().tp, _points.back().tp);
    }

    /** 重置所有记录点 */
    void reset() {
        _points.clear();
        _points.emplace_back("start", Clock::now());
    }

    /** 打印各段耗时：仅 PERF_LOG 定义时生效，否则编译期空操作（每帧零打印） */
    void dump(const std::string& label = "") {
#ifdef PERF_LOG
        if (_points.size() < 2) return;

        std::ostringstream oss;
        if (!label.empty()) oss << "[" << label << "] ";

        double total = 0;
        for (size_t i = 1; i < _points.size(); ++i) {
            double ms = msBetween(_points[i - 1].tp, _points[i].tp);
            total += ms;
            oss << _points[i].name << ":" << std::fixed << std::setprecision(1) << ms << "ms";
            if (i < _points.size() - 1) oss << " | ";
        }
        oss << " | 总计:" << total << "ms";
        std::cout << oss.str() << std::endl;
#else
        (void)label;
#endif
    }

private:
    using Clock = std::chrono::steady_clock;
    using TP    = std::chrono::steady_clock::time_point;

    struct Point {
        std::string name;
        TP tp;
        Point(const std::string& n, TP t) : name(n), tp(t) {}
    };
    std::vector<Point> _points;

    static double msBetween(const TP& a, const TP& b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    }
};
