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
 *   HR0 — TRIGGER : PLC 写 1 触发拍照，Nano 消费后清 0
 *   HR1 — RESULT  : 0=空闲, 1=OK, 2=NG
 *   HR2 — STATUS  : 0=未就绪/故障, 1=就绪（相机掉线/连续空帧时 Nano 置 0，PLC 据此报警停机）
 *   HR3 — DONE    : 完成标志。Nano 写好 HR1 后置 1 通知 PLC"结果可取"，收到应答后清 0。
 *                   只由 Nano 写、PLC 读（PLC 端只建读标签），PLC 不写它。
 *   HR4 — ACK     : 应答标志。PLC 取走 HR1 后写 1 应答，Nano 检测 0→1 边沿后清 HR3 并复位 HR4。
 *                   只由 PLC 写、Nano 读（PLC 端只建写标签）。
 *
 * 为什么拆成两个寄存器：PLC 若对同一个寄存器"又建读标签又建写标签"，
 * 读轮询每周期把寄存器值刷回本地变量，会覆盖程序刚写的应答 → HR3 永远钉 1 卡死。
 * 拆开后 HR3 上 PLC 只读、HR4 上 PLC 只写，方向互不冲突，FStudio 标签各自单向即可。
 *
 * 握手：Nano 置 HR3=1 → PLC 读 HR1 → PLC 写 HR4=1 → Nano 清 HR3、复位 HR4 → 接下一板。
 * HR3=1 期间 Nano 拒绝新触发，防止结果被下一板覆盖 —— 通信不依赖 PLC 定时猜时序。
 *
 * PLC 侧只需配 Modbus TCP 主站，读写对应寄存器即可，无需写 Socket 自由口程序。
 * 触发条件建议加"HR3=0"门禁（上一板结果取走后才允许新触发）。
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

    /** 清空检测结果（HR1 ← 0，空闲），新一板开始时调用，避免 PLC 读到上一板残留结果 */
    bool resetResult();

    /** 置完成标志 HR3 ← 1：结果已写好，PLC 读 HR1 后写 HR4=1 应答。检测完成、sendOK/NG 之后调用 */
    bool setDone();

    /** PLC 是否已应答（HR3 == 0）。为 false 表示上一板结果 PLC 还没取走，不应接收新触发 */
    bool isDoneAcked() const;

    /** 写状态到 HR2（1=就绪, 0=未就绪/故障），相机掉线时置 0，PLC 据此报警停机 */
    bool sendReady(bool ready);

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
    uint16_t               _prev_hr0 = 0;   // HR0 边缘检测：只有 0→1 才触发
    uint16_t               _prev_hr4 = 0;   // HR4 边缘检测：PLC 应答 0→1 边沿
};
