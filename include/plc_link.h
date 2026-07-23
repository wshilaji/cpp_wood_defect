#pragma once

#include <string>
#include <atomic>
#include <thread>
#include <mutex>

/**
 * PLC TCP 通信模块
 *
 * Nano 作为 TCP Server，PLC 作为 Client 连过来：
 *   - PLC → "TRIGGER\n" → Nano 触发相机拍照 + AI 检测
 *   - Nano → "OK\n" 或 "NG\n" → PLC 决定剔除/保留
 */
class PlcLink {
public:
    explicit PlcLink(int port);
    ~PlcLink();

    /** 启动 TCP Server，在后台线程 accept */
    bool start();

    /** 停止服务 */
    void stop();

    /** 是否有 PLC 连上来 */
    bool isConnected() const;

    /** 等待 PLC 发来触发指令（阻塞，-1 表示永久等待） */
    bool waitTrigger(int timeout_ms = -1);

    /** 发送检测结果给 PLC */
    bool sendOK();
    bool sendNG();

private:
    void acceptLoop();

    int                 _port;
    int                 _server_fd = -1;
    int                 _client_fd = -1;
    mutable std::mutex  _client_mutex;
    std::atomic<bool>   _running{false};
    std::thread         _accept_thread;
};
