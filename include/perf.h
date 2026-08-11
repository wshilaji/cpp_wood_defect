#pragma once

/**
 * 性能计时工具 — 联调时打开 PERF_LOG，生产关闭
 *
 * 用法:
 *   PerfTimer t;
 *   t.tick("拍照");
 *   t.tick("增强");
 *   t.tick("推理");
 *   t.tick("后处理");
 *   t.tick("PLC发送");
 *   t.dump("第N次");   // 打印各段耗时 + 总计
 */

// ========== 开关：注释掉下面这行即关闭所有耗时打印 ==========
#define PERF_LOG

#ifdef PERF_LOG

#include <chrono>
#include <string>
#include <vector>
#include <iostream>
#include <iomanip>
#include <sstream>

class PerfTimer {
public:
    PerfTimer() { _points.emplace_back("start", Clock::now()); }

    /** 记录一个时间点 */
    void tick(const std::string& name) {
        _points.emplace_back(name, Clock::now());
    }

    /** 打印各段耗时 */
    void dump(const std::string& label = "") {
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
    }

    /** 从第一个点到最后一个点的总耗时 (ms) */
    double elapsed() const {
        if (_points.size() < 2) return 0;
        return msBetween(_points.front().tp, _points.back().tp);
    }

    /** 重置所有记录点 */
    void reset() {
        _points.clear();
        _points.emplace_back("start", Clock::now());
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

#else  // PERF_LOG 关闭 → 全部编译为空操作

class PerfTimer {
public:
    void tick(const std::string&) {}
    void dump(const std::string& = "") {}
    double elapsed() const { return 0; }
    void reset() {}
};

#endif
