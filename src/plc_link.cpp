#include "plc_link.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <cerrno>
#include <poll.h>

// ============================================================
// 构造 / 析构
// ============================================================
PlcLink::PlcLink(int port) : _port(port) {}

PlcLink::~PlcLink() {
    stop();
}

// ============================================================
// 启动 TCP Server
// ============================================================
bool PlcLink::start() {
    _server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (_server_fd < 0) {
        std::cerr << "[PLC] 创建 socket 失败: " << strerror(errno) << std::endl;
        return false;
    }

    int opt = 1;
    setsockopt(_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(_port);

    if (bind(_server_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "[PLC] bind 失败: " << strerror(errno) << std::endl;
        close(_server_fd);
        _server_fd = -1;
        return false;
    }

    if (listen(_server_fd, 1) < 0) {
        std::cerr << "[PLC] listen 失败: " << strerror(errno) << std::endl;
        close(_server_fd);
        _server_fd = -1;
        return false;
    }

    _running.store(true);
    _accept_thread = std::thread(&PlcLink::acceptLoop, this);

    std::cout << "[PLC] TCP Server 已启动, 端口=" << _port
              << ", 等待 PLC 连接..." << std::endl;
    return true;
}

// ============================================================
// 停止
// ============================================================
void PlcLink::stop() {
    _running.store(false);

    // 关闭 client
    {
        std::lock_guard<std::mutex> lock(_client_mutex);
        if (_client_fd >= 0) {
            shutdown(_client_fd, SHUT_RDWR);
            close(_client_fd);
            _client_fd = -1;
        }
    }

    // 关闭 server
    if (_server_fd >= 0) {
        shutdown(_server_fd, SHUT_RDWR);
        close(_server_fd);
        _server_fd = -1;
    }

    if (_accept_thread.joinable()) {
        _accept_thread.join();
    }

    std::cout << "[PLC] TCP Server 已停止" << std::endl;
}

// ============================================================
// accept 后台线程
// ============================================================
void PlcLink::acceptLoop() {
    while (_running.load()) {
        // 用 poll 等待连接，超时 1s 以便检查 _running
        pollfd pfd{};
        pfd.fd     = _server_fd;
        pfd.events = POLLIN;

        int ret = poll(&pfd, 1, 1000);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ret == 0) continue;           // 超时，重试

        sockaddr_in client_addr{};
        socklen_t   addr_len = sizeof(client_addr);
        int fd = accept(_server_fd, (sockaddr*)&client_addr, &addr_len);
        if (fd < 0) {
            if (!_running.load()) break;
            continue;
        }

        // 断开旧连接
        {
            std::lock_guard<std::mutex> lock(_client_mutex);
            if (_client_fd >= 0) {
                shutdown(_client_fd, SHUT_RDWR);
                close(_client_fd);
            }
            _client_fd = fd;
        }

        // TCP keepalive，防止 PLC 断电断连后 Nano 不知道
        int ka = 1, ka_idle = 5, ka_intvl = 2, ka_cnt = 3;
        setsockopt(fd, SOL_SOCKET,  SO_KEEPALIVE, &ka,       sizeof(ka));
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE,  &ka_idle,  sizeof(ka_idle));
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &ka_intvl, sizeof(ka_intvl));
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT,   &ka_cnt,   sizeof(ka_cnt));

        std::cout << "[PLC] 客户端已连接: " << inet_ntoa(client_addr.sin_addr)
                  << ":" << ntohs(client_addr.sin_port) << std::endl;
    }
}

// ============================================================
// 是否已连接
// ============================================================
bool PlcLink::isConnected() const {
    std::lock_guard<std::mutex> lock(_client_mutex);
    return _client_fd >= 0;
}

// ============================================================
// 等待 PLC 触发指令
// ============================================================
bool PlcLink::waitTrigger(int timeout_ms) {
    int fd;
    {
        std::lock_guard<std::mutex> lock(_client_mutex);
        fd = _client_fd;
    }

    if (fd < 0) return false;

    pollfd pfd{};
    pfd.fd     = fd;
    pfd.events = POLLIN;

    int ret = poll(&pfd, 1, timeout_ms);
    if (ret <= 0) return false;          // 超时或错误

    char buf[256];
    int n = recv(fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) {
        // PLC 断开
        std::lock_guard<std::mutex> lock(_client_mutex);
        close(_client_fd);
        _client_fd = -1;
        std::cerr << "[PLC] 连接断开" << std::endl;
        return false;
    }

    buf[n] = '\0';
    std::cout << "[PLC] 收到触发指令: " << buf << std::endl;
    return true;
}

// ============================================================
// 发送结果
// ============================================================
bool PlcLink::sendOK() {
    std::lock_guard<std::mutex> lock(_client_mutex);
    if (_client_fd < 0) return false;
    int n = send(_client_fd, "OK\n", 3, MSG_NOSIGNAL);
    return n == 3;
}

bool PlcLink::sendNG() {
    std::lock_guard<std::mutex> lock(_client_mutex);
    if (_client_fd < 0) return false;
    int n = send(_client_fd, "NG\n", 3, MSG_NOSIGNAL);
    return n == 3;
}
