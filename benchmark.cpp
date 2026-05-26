/**
 * benchmark.cpp - 注册/登录性能压测工具
 *
 * 支持：
 *   - 短连接 / Keep-Alive 长连接
 *   - 每线程多个并发连接（-c 参数）
 *   - 用法: ./benchmark -p 9090 -t 8 -c 8 -n 1000 -m register -k
 */

#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <string>
#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <getopt.h>
#include <sstream>

std::atomic<int64_t> g_success{0};
std::atomic<int64_t> g_failed{0};
std::atomic<int64_t> g_latency_us{0};
std::atomic<int64_t> g_next_id{0};
int64_t g_total_target = 0;

inline void set_nb(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

inline int do_connect(const char* host, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int yes = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host, &addr.sin_addr);
    if (connect(fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

struct ConnState {
    int fd = -1; // socket 文件描述符
    int uid = -1; // 当前请求的用户 ID（用于生成唯一用户名）
    std::string send_buf; // 待发送的 HTTP 请求
    size_t send_off = 0;  // 已发偏移量
    std::string recv_buf; //  接收缓冲区
    bool waiting_response = false; //标记是否已发完请求、等待响应
    std::chrono::high_resolution_clock::time_point t0; // 请求开始时间（用于计算延迟）
};

// 非阻塞写
bool try_send(ConnState& c) {
    while (c.send_off < c.send_buf.size()) {
        ssize_t n = write(c.fd, c.send_buf.data() + c.send_off,
                          c.send_buf.size() - c.send_off);
        if (n > 0) { c.send_off += n; continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;
        return false;
    }
    return true;
}

// 检查响应是否完整（解析 Content-Length）
int check_response(const std::string& raw, std::string& body) {
    size_t hdr_end = raw.find("\r\n\r\n");
    if (hdr_end == std::string::npos) return 0; // 未完整

    std::string hdr = raw.substr(0, hdr_end);
    size_t cl_pos = hdr.find("Content-Length: ");
    if (cl_pos == std::string::npos) cl_pos = hdr.find("content-length: ");
    if (cl_pos == std::string::npos) return -1;

    size_t vs = cl_pos + 16;
    size_t ve = hdr.find("\r\n", vs);
    size_t cl = std::stoul(hdr.substr(vs, ve - vs));
    size_t total = hdr_end + 4 + cl;
    if (raw.size() < total) return 0;

    body = raw.substr(hdr_end + 4, cl);
    return (int)total;
}

void worker_epoll(const char* host, int port, const std::string& mode,
                  int conns_per_thread, bool keep_alive) {
    std::string path = (mode == "register") ? "/api/register" : "/api/login";
    std::string ok_marker = (mode == "register") ? "注册成功" : "token";

    int epoll_fd = epoll_create1(0);
    std::vector<ConnState> conns(conns_per_thread);

    auto build_request = [&](int uid) -> std::string {
        std::string body = "{\"username\":\"bench_user_" + std::to_string(uid)
                         + "\",\"password\":\"pass" + std::to_string(uid) + "\"}";
        std::ostringstream req;
        req << "POST " << path << " HTTP/1.1\r\n"
            << "Host: " << host << "\r\n"
            << "Content-Type: application/json\r\n"
            << "Content-Length: " << body.size() << "\r\n"
            << "Connection: " << (keep_alive ? "keep-alive" : "close") << "\r\n"
            << "\r\n" << body;
        return req.str();
    };

    auto setup_conn = [&](ConnState& c) -> bool {
        int64_t uid = g_next_id.fetch_add(1, std::memory_order_relaxed);
        if (uid >= g_total_target) { c.fd = -1; return false; }

        bool is_new = (c.fd < 0);
        if (is_new) {
            c.fd = do_connect(host, port);
            if (c.fd < 0) { g_failed++; return false; }
            set_nb(c.fd);
            epoll_event ev{};
            ev.events = EPOLLOUT | EPOLLET;
            ev.data.ptr = &c;
            epoll_ctl(epoll_fd, EPOLL_CTL_ADD, c.fd, &ev);
        } else {
            // Keep-Alive 复用：切回 EPOLLOUT 模式发送下一个请求
            epoll_event ev{};
            ev.events = EPOLLOUT | EPOLLET;
            ev.data.ptr = &c;
            epoll_ctl(epoll_fd, EPOLL_CTL_MOD, c.fd, &ev);
        }

        c.uid = (int)uid;
        c.send_buf = build_request(c.uid);
        c.send_off = 0;
        c.recv_buf.clear();
        c.waiting_response = false;
        c.t0 = std::chrono::high_resolution_clock::now();
        return true;
    };

    auto close_conn = [&](ConnState& c) {
        if (c.fd >= 0) {
            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, c.fd, nullptr);
            close(c.fd);
            c.fd = -1;
        }
    };

    // 记录延迟、统计成功失败、触发下一个请求
    auto finish_and_next = [&](ConnState& c, bool success) {
        auto t1 = std::chrono::high_resolution_clock::now();
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - c.t0).count();
        g_latency_us.fetch_add(us, std::memory_order_relaxed);
        if (success) g_success.fetch_add(1, std::memory_order_relaxed);
        else g_failed.fetch_add(1, std::memory_order_relaxed);

        if (!keep_alive) {
            close_conn(c);
        }
        // 准备下一个请求
        setup_conn(c);
    };

    // 初始化所有连接
    for (auto& c : conns) {
        setup_conn(c);
    }

    epoll_event events[256];
    while (g_success + g_failed < g_total_target) {
        int n = epoll_wait(epoll_fd, events, 256, 100);
        for (int i = 0; i < n; ++i) {
            ConnState* c = (ConnState*)events[i].data.ptr;
            if (!c || c->fd < 0) continue;
            uint32_t ev = events[i].events;

            if (ev & (EPOLLERR | EPOLLHUP)) {
                close_conn(*c);
                g_failed++;
                setup_conn(*c);
                continue;
            }

            // 写事件：发送请求
            if ((ev & EPOLLOUT) && !c->waiting_response) {
                if (!try_send(*c)) {
                    close_conn(*c);
                    g_failed++;
                    setup_conn(*c);
                    continue;
                }
                if (c->send_off == c->send_buf.size()) {
                    c->waiting_response = true;
                    epoll_event mod{};
                    mod.events = EPOLLIN | EPOLLET;
                    mod.data.ptr = c;
                    epoll_ctl(epoll_fd, EPOLL_CTL_MOD, c->fd, &mod);
                }
            }

            // 读事件：接收响应
            if ((ev & EPOLLIN) && c->waiting_response) {
                char buf[4096];
                bool got_response = false;
                while (true) {
                    ssize_t r = read(c->fd, buf, sizeof(buf));
                    if (r > 0) {
                        c->recv_buf.append(buf, r);
                        std::string body;
                        int consumed = check_response(c->recv_buf, body);
                        if (consumed > 0) {
                            bool ok = body.find(ok_marker) != std::string::npos;
                            finish_and_next(*c, ok);
                            got_response = true;
                            break;
                        }
                        continue;
                    }
                    if (r == 0) { close_conn(*c); g_failed++; setup_conn(*c); got_response = true; break; }
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    if (errno == EINTR) continue;
                    close_conn(*c); g_failed++; setup_conn(*c); got_response = true; break;
                }
                (void)got_response;
            }
        }
    }

    for (auto& c : conns) close_conn(c);
    close(epoll_fd);
}

int main(int argc, char* argv[]) {
    const char* host = "127.0.0.1";
    int port = 9090;
    int num_threads = 4;
    int conns_per_thread = 1;
    int64_t total_requests = 1000;
    std::string mode = "register";
    bool keep_alive = false;

    int opt;
    while ((opt = getopt(argc, argv, "h:p:t:c:n:m:k")) != -1) {
        switch (opt) {
            case 'h': host = optarg; break;
            case 'p': port = std::stoi(optarg); break;
            case 't': num_threads = std::stoi(optarg); break;
            case 'c': conns_per_thread = std::stoi(optarg); break;
            case 'n': total_requests = std::stoll(optarg); break;
            case 'm': mode = optarg; break;
            case 'k': keep_alive = true; break;
        }
    }

    g_total_target = total_requests;
    int total_conns = num_threads * conns_per_thread;

    std::cout << "========================================" << std::endl;
    std::cout << "HTTP Benchmark: " << mode
              << (keep_alive ? " [Keep-Alive]" : " [Short]") << std::endl;
    std::cout << "  Target:       " << host << ":" << port << std::endl;
    std::cout << "  Threads:      " << num_threads << std::endl;
    std::cout << "  Conn/Thread:  " << conns_per_thread << std::endl;
    std::cout << "  Total Conns:  " << total_conns << std::endl;
    std::cout << "  Requests:     " << total_requests << std::endl;
    std::cout << "========================================" << std::endl;

    auto start = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back(worker_epoll, host, port, mode, conns_per_thread, keep_alive);
    }
    for (auto& t : threads) t.join();

    auto end = std::chrono::high_resolution_clock::now();
    double dur = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count() / 1000.0;
    int64_t done = g_success + g_failed;
    double qps = done / dur;
    double lat = done > 0 ? (double)g_latency_us / done / 1000.0 : 0;

    std::cout << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "Results:" << std::endl;
    std::cout << "  Duration:     " << std::fixed << std::setprecision(2) << dur << " s" << std::endl;
    std::cout << "  Success:      " << g_success << std::endl;
    std::cout << "  Failed:       " << g_failed << std::endl;
    std::cout << "  QPS:          " << std::fixed << std::setprecision(0) << qps << " req/s" << std::endl;
    std::cout << "  Avg Latency:  " << std::fixed << std::setprecision(2) << lat << " ms" << std::endl;
    std::cout << "========================================" << std::endl;

    return 0;
}
