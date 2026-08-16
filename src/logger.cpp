#include "logger.h"

#include <chrono>
#include <ctime>
#include <cstring>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

constexpr const char* LOG_DIR  = "./output/logs";
constexpr const char* LOG_FILE = "./output/logs/app.log";
constexpr const char* LOG_BAK  = "./output/logs/app.log.1";
constexpr long        MAX_BYTES = 5L * 1024 * 1024;   // 5MB 轮转

// 级别标签
const char* levelName(LogLevel lv) {
    switch (lv) {
        case LogLevel::INFO: return "INFO";
        case LogLevel::WARN: return "WARN";
        case LogLevel::ERR:  return "ERR ";
        default:             return "?   ";
    }
}

// 时间戳 [YYYY-MM-DD HH:MM:SS.mmm]
std::string timestamp() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()) % 1000;

    char buf[32];
    std::tm tm{};
    localtime_r(&t, &tm);
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tm);
    char out[40];
    std::snprintf(out, sizeof out, "%s.%03d", buf, static_cast<int>(ms.count()));
    return out;
}

} // namespace

// ============================================================
// LogStream: 流式代理, 析构时落盘
// ============================================================
LogStream::LogStream(LogLevel lv) : _lv(lv) {}
LogStream::LogStream(LogStream&& o) noexcept : _lv(o._lv), _ss(std::move(o._ss)) {}

LogStream::~LogStream() {
    Logger::inst().write(_lv, _ss.str());
}

LogStream& LogStream::operator<<(std::ostream& (*f)(std::ostream&)) {
    _ss << f;   // std::endl / std::flush
    return *this;
}

// ============================================================
// Logger
// ============================================================
Logger& Logger::inst() {
    static Logger s;
    return s;
}

Logger::Logger() {
    // 确保目录存在（幂等）。mkdir 只建一层, 父目录不存在会失败(ENOENT), 要分开建
    ::mkdir("./output", 0755);
    ::mkdir(LOG_DIR, 0755);
    openFile();
}

Logger::~Logger() {
    if (_fp) { std::fclose(_fp); _fp = nullptr; }
}

void Logger::openFile() {
    if (_fp) { std::fclose(_fp); _fp = nullptr; }
    _fp = std::fopen(LOG_FILE, "a");           // 追加写
    if (_fp) {
        std::fseek(_fp, 0, SEEK_END);
        _bytes = std::ftell(_fp);
    } else {
        // 文件打不开: 退化为只打 stdout, 不崩
        _bytes = 0;
    }
}

void Logger::rotateIfNeeded() {
    if (_bytes < MAX_BYTES) return;
    // 超限: 当前日志滚成 .1(覆盖旧备份), 重新开空文件
    if (_fp) { std::fclose(_fp); _fp = nullptr; }
    std::rename(LOG_FILE, LOG_BAK);
    openFile();
}

void Logger::write(LogLevel lv, const std::string& msg) {
    std::lock_guard<std::mutex> lock(_mtx);

    std::string line = "[" + timestamp() + "] [" + levelName(lv) + "] " + msg;

    // 落盘
    if (_fp) {
        std::fprintf(_fp, "%s\n", line.c_str());
        std::fflush(_fp);                       // 立即刷盘, 崩溃/断电也不丢
        _bytes += static_cast<long>(line.size()) + 1;
        rotateIfNeeded();
    }

    // 镜像 stdout（systemd 下 journalctl 也收一份）
    std::fprintf(stdout, "%s\n", line.c_str());
    std::fflush(stdout);
}
