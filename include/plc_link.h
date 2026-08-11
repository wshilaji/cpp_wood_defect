#pragma once

#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <modbus.h>

/**
 * PLC Modbus TCP 通信模块
 *
 * Nano 作为 Modbus TCP Server（从站），PLC（繁易 FC5）作为 Client（主站）。
 *
 * Holding Register 映射:
 *   HR0 — TRIGGER : PLC 写 1 触发拍照，Nano 检测完后清 0
 *   HR1 — RESULT   : 0=空闲, 1=OK, 2=NG
 *   HR2 — STATUS   : 0=未就绪, 1=就绪
 *
 * PLC 侧只需配 Modbus TCP 主站，读写对应寄存器即可，无需写 Socket 自由口程序。
 */
class PlcLink {
public:
    explicit PlcLink(int port = 502);
    ~PlcLink();

    /** 启动 Modbus TCP Server，后台线程处理请求 */
    bool start();

    /** 停止服务 */
    void stop();

    /** 是否有 PLC 连上来（最近一次请求是否活跃） */
    bool isConnected() const;

    /**
     * 等待 PLC 写 HR0=1（条件变量通知，零延迟）
     * @param timeout_ms 超时毫秒，-1 表示永久等待
     * @return true=收到触发，false=超时或停止
     */
    bool waitTrigger(int timeout_ms = -1);

    /** 写检测结果到 HR1 */
    bool sendOK();    // HR1 ← 1
    bool sendNG();    // HR1 ← 2

private:
    void serverLoop();

    modbus_t*              _ctx          = nullptr;
    modbus_mapping_t*      _mb_mapping   = nullptr;
    int                    _server_socket = -1;
    int                    _port;
    std::atomic<bool>      _running{false};
    std::atomic<bool>      _client_active{false};
    std::thread            _server_thread;
    mutable std::mutex     _mapping_mutex;

    // 事件驱动：条件变量替代轮询
    std::mutex             _trigger_mutex;
    std::condition_variable _trigger_cv;
    std::atomic<bool>      _trigger_pending{false};
    std::atomic<bool>      _waiting{false};
    uint16_t               _prev_hr0 = 0;   // 边缘检测：只有 0→1 才触发
};
