#pragma once

#include <sstream>
#include <string>
#include <mutex>
#include <cstdio>

/**
 * 轻量落盘日志
 *
 * 全屏 kiosk 界面没人看控制台; 程序错误/警告除镜像到 stdout(systemd 会进
 * journalctl)外, 还必须落盘到 output/logs/app.log, 现场直接翻文件即可。
 *
 * 用法(流式, 用法和 std::cerr 一样, 自动补时间戳/级别/换行):
 *   LOGI << "Server 已启动, 端口=" << _port;
 *   LOGW << "上一板结果未应答(HR3=1), 忽略本次触发";
 *   LOGE << "modbus_new_tcp 失败: " << modbus_strerror(errno);
 *
 * 线程安全: 内部互斥锁, 任意线程可调。
 * 轮转: 单文件超 5MB 自动改名 app.log.1(覆盖旧备份), 防无限增长。
 */
enum class LogLevel { INFO, WARN, ERR };

// 流式日志代理: << 累积内容, 析构时落盘(自动补级别/时间戳/换行)
class LogStream {
public:
    explicit LogStream(LogLevel lv);
    LogStream(LogStream&& o) noexcept;
    ~LogStream();

    template <typename T>
    LogStream& operator<<(const T& v) { _ss << v; return *this; }
    LogStream& operator<<(std::ostream& (*f)(std::ostream&));   // 兼容 std::endl

private:
    LogLevel       _lv;
    std::ostringstream _ss;
};

#define LOGI LogStream(LogLevel::INFO)
#define LOGW LogStream(LogLevel::WARN)
#define LOGE LogStream(LogLevel::ERR)

class Logger {
public:
    static Logger& inst();

    /** 线程安全: 写入 app.log + 镜像 stdout */
    void write(LogLevel lv, const std::string& msg);

private:
    Logger();
    ~Logger();
    void openFile();
    void rotateIfNeeded();

    FILE*     _fp    = nullptr;
    long      _bytes = 0;      // 已写字节, 用于轮转
    std::mutex _mtx;
};
