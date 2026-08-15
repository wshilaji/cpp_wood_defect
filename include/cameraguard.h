#pragma once

#include <chrono>
#include <functional>

/**
 * 相机守护：负责相机掉线自动重连 + 连续空帧故障判定。
 *
 * 解耦设计：不依赖相机 SDK / PLC / Qt，只通过两个回调与外部对接——
 *   - connect  执行一次完整连接（stop → 枚举打开 → start），返回是否成功
 *   - on_state 状态变化通知（true=已就绪, false=判定故障），由 main() 接回调去写 PLC HR2 / 界面灯
 *
 * 用法（主循环单线程驱动，与项目风格一致）：
 *   CameraGuard guard(connectFn, notifyFn, initialRunning);
 *   guard.poll();      // 每轮循环调用：未连接则限频重连（默认 1s 一次）
 *   guard.onFrame();   // 每次拍照正常拿到帧
 *   guard.onMiss();    // 每次空帧；返回 true = 刚判定故障（调用方应立即停相机，下轮 poll 自动重连）
 */
class CameraGuard {
public:
    using ConnectFn = std::function<bool()>;           // 完整连接，成功返回 true
    using NotifyFn  = std::function<void(bool ready)>; // 状态变化：true=就绪, false=故障

    CameraGuard(ConnectFn connect, NotifyFn on_state, bool running = false)
        : _connect(std::move(connect)), _on_state(std::move(on_state)), _running(running) {}

    /** 是否认为相机可用（连接中且未判定故障） */
    bool running() const { return _running; }

    /** 主循环每轮调用：未连接 → 限频后台重连，成功后通知 on_state(true) */
    void poll() {
        if (_running) return;
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(now - _last_attempt).count() < RECONNECT_INTERVAL_S) return;
        _last_attempt = now;
        if (_connect()) {
            _running = true;
            _misses = 0;
            _on_state(true);
        }
    }

    /** 一次拍照正常拿到帧：清除连续空帧计数 */
    void onFrame() { _misses = 0; }

    /**
     * 一次拍照空帧：累计连续空帧数，达到阈值判定故障并通知 on_state(false)。
     * @return true = 本次刚判定故障（调用方应立即停相机，下轮 poll 会自动重连）
     */
    bool onMiss() {
        if (++_misses < MAX_MISSES) return false;
        _misses = 0;
        _running = false;
        _on_state(false);
        return true;
    }

private:
    static constexpr double RECONNECT_INTERVAL_S = 1.0;   // 重连限频 1 秒
    static constexpr int    MAX_MISSES          = 3;       // 连续 N 次空帧判定故障

    ConnectFn _connect;
    NotifyFn  _on_state;
    bool      _running = false;
    int       _misses  = 0;
    std::chrono::steady_clock::time_point _last_attempt{};
};
