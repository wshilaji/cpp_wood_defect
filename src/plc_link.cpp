#include "plc_link.h"
#include "logger.h"

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
        LOGE << "[PLC] modbus_new_tcp 失败: " << modbus_strerror(errno);
        return false;
    }

    // 从站 ID = 1（FC5 主站配置里填 1）
    modbus_set_slave(_ctx, 1);

    // 调试关闭（生产环境不开，避免刷屏）
    modbus_set_debug(_ctx, 0);

    // 分配寄存器映射: 0 coil, 0 discrete input, 5 holding registers, 0 input registers
    _mb_mapping = modbus_mapping_new(0, 0, 5, 0);
    if (!_mb_mapping) {
        LOGE << "[PLC] modbus_mapping_new 失败: " << modbus_strerror(errno);
        modbus_free(_ctx);
        _ctx = nullptr;
        return false;
    }

    // 初始化寄存器值
    _mb_mapping->tab_registers[0] = 0;   // HR0: 触发（0=空闲）
    _mb_mapping->tab_registers[1] = 0;   // HR1: 结果（0=空闲）
    _mb_mapping->tab_registers[2] = 1;   // HR2: 状态（1=就绪）
    _mb_mapping->tab_registers[3] = 0;   // HR3: 完成标志（0=空闲, 1=结果已写好等 PLC 取走）
    _mb_mapping->tab_registers[4] = 0;   // HR4: 应答（PLC 取走结果后写 1）

    // 初始化边缘检测基准，避免启动时如果 PLC 已经写 1 产生误触发
    _prev_hr0 = _mb_mapping->tab_registers[0];
    _prev_hr4 = _mb_mapping->tab_registers[4];
    _trigger_pending.store(false);
    _waiting.store(false);

    _running.store(true);
    _server_thread = std::thread(&PlcLink::serverLoop, this);

    LOGI << "[PLC] Modbus TCP Server 已启动, 端口=" << _port
         << ", 从站ID=1, 等待 PLC 连接...";
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

    LOGI << "[PLC] Modbus TCP Server 已停止";
}

// ============================================================
// 后台线程：listen → accept → 请求处理循环
// ============================================================
void PlcLink::serverLoop() {
    _server_socket = modbus_tcp_listen(_ctx, 1);  // backlog=1，单客户端
    if (_server_socket == -1) {
        LOGE << "[PLC] modbus_tcp_listen 失败: " << modbus_strerror(errno);
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
        LOGI << "[PLC] Modbus TCP 客户端已连接";

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

                // HR4 应答检测：PLC 写 HR4 0→1 表示结果已取走，清完成标志并复位 HR4。
                // 复位 HR4 让下个应答也是 0→1 边沿；HR3=0 时也复位，防漏边沿。
                if (_mb_mapping) {
                    uint16_t ack = _mb_mapping->tab_registers[4];
                    if (_prev_hr4 == 0 && ack == 1) {
                        if (_mb_mapping->tab_registers[3] == 1) {
                            _mb_mapping->tab_registers[3] = 0;   // 清完成标志 → 解锁下一触发
                            // TODO(TEMP): 采样日志, 观察握手是否工作, 确认后注释掉
                            static unsigned ack_cnt = 0;
                            if (++ack_cnt % 2 == 0)
                                LOGI << "[PLC] 应答采样: 第 " << ack_cnt
                                     << " 次应答, HR3 已清除";
                        }
                        _mb_mapping->tab_registers[4] = 0;       // 复位应答，等下个 0→1
                        _prev_hr4 = 0;
                    } else {
                        _prev_hr4 = ack;
                    }
                }
            }
        }

        _client_active.store(false);
        LOGW << "[PLC] Modbus TCP 客户端断开";
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
            // 握手门禁: 上一板结果 PLC 还没应答(HR3=1)时, 拒绝新触发,
            // 防止 HR1 被下一板结果覆盖导致 PLC 读到错的结果
            if (_mb_mapping->tab_registers[3] != 0) {
                LOGW << "[PLC] 上一板结果未应答(HR3=1), 忽略本次触发";
                return false;
            }
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
            // 握手门禁(同快速路径): 上一板结果未应答则拒绝本次触发
            if (_mb_mapping->tab_registers[3] != 0) {
                LOGW << "[PLC] 上一板结果未应答(HR3=1), 忽略本次触发";
                return false;
            }
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
// 完成标志 HR3（握手核心）
// ============================================================
bool PlcLink::setDone() {
    std::lock_guard<std::mutex> lock(_mapping_mutex);
    if (!_mb_mapping) return false;
    _mb_mapping->tab_registers[3] = 1;    // 1 = 结果已写好，PLC 可读 HR1；读后写 0 应答
    return true;
}

bool PlcLink::isDoneAcked() const {
    std::lock_guard<std::mutex> lock(_mapping_mutex);
    return !_mb_mapping || _mb_mapping->tab_registers[3] == 0;
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
