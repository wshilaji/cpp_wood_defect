#include "plc_link.h"

#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <cerrno>
#include <thread>
#include <chrono>

// ============================================================
// 构造 / 析构
// ============================================================
PlcLink::PlcLink(int port) : _port(port) {}

PlcLink::~PlcLink() {
    stop();
}

// ============================================================
// 启动 Modbus TCP Server
// ============================================================
bool PlcLink::start() {
    _ctx = modbus_new_tcp(nullptr, _port);  // nullptr → 监听所有网口
    if (!_ctx) {
        std::cerr << "[PLC] modbus_new_tcp 失败: " << modbus_strerror(errno) << std::endl;
        return false;
    }

    // 从站 ID = 1（FC5 主站配置里填 1）
    modbus_set_slave(_ctx, 1);

    // 调试关闭（生产环境不开，避免刷屏）
    modbus_set_debug(_ctx, 0);

    // 分配寄存器映射: 0 coil, 0 discrete input, 3 holding registers, 0 input registers
    _mb_mapping = modbus_mapping_new(0, 0, 3, 0);
    if (!_mb_mapping) {
        std::cerr << "[PLC] modbus_mapping_new 失败: " << modbus_strerror(errno) << std::endl;
        modbus_free(_ctx);
        _ctx = nullptr;
        return false;
    }

    // 初始化寄存器值
    _mb_mapping->tab_registers[0] = 0;   // HR0: 触发（0=空闲）
    _mb_mapping->tab_registers[1] = 0;   // HR1: 结果（0=空闲）
    _mb_mapping->tab_registers[2] = 1;   // HR2: 状态（1=就绪）

    // 初始化边缘检测基准，避免启动时如果 PLC 已经写 1 产生误触发
    _prev_hr0 = _mb_mapping->tab_registers[0];
    _trigger_pending.store(false);
    _waiting.store(false);

    _running.store(true);
    _server_thread = std::thread(&PlcLink::serverLoop, this);

    std::cout << "[PLC] Modbus TCP Server 已启动, 端口=" << _port
              << ", 从站ID=1, 等待 PLC 连接..." << std::endl;
    return true;
}

// ============================================================
// 停止
// ============================================================
void PlcLink::stop() {
    _running.store(false);

    // 唤醒可能正在 waitTrigger 的线程
    {
        std::lock_guard<std::mutex> lock(_trigger_mutex);
        _trigger_pending.store(true);
    }
    _trigger_cv.notify_all();

    // 1. 关闭监听 socket — 中断 modbus_tcp_accept 阻塞
    if (_server_socket >= 0) {
        shutdown(_server_socket, SHUT_RDWR);
        close(_server_socket);
        _server_socket = -1;
    }

    // 2. 关闭 modbus 连接 — 中断 modbus_receive 阻塞
    if (_ctx) {
        modbus_close(_ctx);
    }

    // 3. 等待线程退出
    if (_server_thread.joinable()) {
        _server_thread.join();
    }

    // 4. 释放资源
    if (_mb_mapping) {
        modbus_mapping_free(_mb_mapping);
        _mb_mapping = nullptr;
    }
    if (_ctx) {
        modbus_free(_ctx);
        _ctx = nullptr;
    }

    std::cout << "[PLC] Modbus TCP Server 已停止" << std::endl;
}

// ============================================================
// 后台线程：listen → accept → 请求处理循环
// ============================================================
void PlcLink::serverLoop() {
    _server_socket = modbus_tcp_listen(_ctx, 1);  // backlog=1，单客户端
    if (_server_socket == -1) {
        std::cerr << "[PLC] modbus_tcp_listen 失败: "
                  << modbus_strerror(errno) << std::endl;
        return;
    }

    while (_running.load()) {
        // 等待 PLC 连接（阻塞，stop() 时 close(_server_socket) 会中断）
        int rc = modbus_tcp_accept(_ctx, &_server_socket);
        if (rc == -1) {
            if (!_running.load()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        _client_active.store(true);
        std::cout << "[PLC] Modbus TCP 客户端已连接" << std::endl;

        // 请求处理循环
        while (_running.load()) {
            uint8_t query[MODBUS_TCP_MAX_ADU_LENGTH];
            int len = modbus_receive(_ctx, query);
            if (len == -1) {
                // 客户端断开或出错
                break;
            }

            // 处理请求（读写映射）
            {
                std::lock_guard<std::mutex> lock(_mapping_mutex);
                modbus_reply(_ctx, query, len, _mb_mapping);

                // 边缘检测：HR0 0→1 时通知主线程（事件驱动，零延迟）
                if (_waiting.load() && _mb_mapping) {
                    uint16_t cur = _mb_mapping->tab_registers[0];
                    if (_prev_hr0 == 0 && cur == 1) {
                        bool expected = false;
                        if (_trigger_pending.compare_exchange_strong(expected, true)) {
                            _trigger_cv.notify_one();
                        }
                    }
                    _prev_hr0 = cur;
                }
            }
        }

        _client_active.store(false);
        std::cout << "[PLC] Modbus TCP 客户端断开" << std::endl;
    }
}

// ============================================================
// 是否活跃
// ============================================================
bool PlcLink::isConnected() const {
    return _client_active.load();
}

// ============================================================
// 等待 PLC 触发（条件变量，零延迟事件驱动）
// ============================================================
bool PlcLink::waitTrigger(int timeout_ms) {
    // 快速检查：是否已经有触发信号（避免漏掉条件变量通知之前的写入）
    {
        std::lock_guard<std::mutex> lock(_mapping_mutex);
        if (_mb_mapping && _prev_hr0 == 0 && _mb_mapping->tab_registers[0] == 1) {
            _mb_mapping->tab_registers[0] = 0;
            _prev_hr0 = 0;
            return true;
        }
    }

    _waiting.store(true);

    {
        std::unique_lock<std::mutex> lock(_trigger_mutex);
        if (timeout_ms < 0) {
            _trigger_cv.wait(lock, [this]{
                return _trigger_pending.load() || !_running.load();
            });
        } else {
            bool got = _trigger_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this]{
                return _trigger_pending.load() || !_running.load();
            });
            if (!got) {
                _waiting.store(false);
                return false;  // 超时
            }
        }
    }

    _waiting.store(false);

    if (!_running.load()) return false;

    bool triggered = _trigger_pending.exchange(false);
    if (triggered) {
        std::lock_guard<std::mutex> lock(_mapping_mutex);
        if (_mb_mapping) {
            _mb_mapping->tab_registers[0] = 0;
            _prev_hr0 = 0;  // 重置边缘检测，等待下一个 0→1
        }
    }

    return triggered;
}

// ============================================================
// 写检测结果到 HR1
// ============================================================
bool PlcLink::sendOK() {
    std::lock_guard<std::mutex> lock(_mapping_mutex);
    if (!_mb_mapping) return false;
    _mb_mapping->tab_registers[1] = 1;    // 1 = OK
    return true;
}

bool PlcLink::sendNG() {
    std::lock_guard<std::mutex> lock(_mapping_mutex);
    if (!_mb_mapping) return false;
    _mb_mapping->tab_registers[1] = 2;    // 2 = NG
    return true;
}

bool PlcLink::resetResult() {
    std::lock_guard<std::mutex> lock(_mapping_mutex);
    if (!_mb_mapping) return false;
    _mb_mapping->tab_registers[1] = 0;    // 0 = 空闲
    return true;
}

// ============================================================
// 写状态到 HR2（1=就绪, 0=未就绪/故障）
// ============================================================
bool PlcLink::sendReady(bool ready) {
    std::lock_guard<std::mutex> lock(_mapping_mutex);
    if (!_mb_mapping) return false;
    _mb_mapping->tab_registers[2] = ready ? 1 : 0;
    return true;
}
