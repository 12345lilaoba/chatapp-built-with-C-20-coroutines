/**
 * ws_bench.cpp - WebSocket 压力测试工具
 *
 * 测试维度：
 *   1. 连接容量：大量并发 WebSocket 连接
 *   2. 消息吞吐量：每连接发送消息，测广播 QPS
 *   3. 延迟分布：发送 -> 收到自己广播的延迟
 *
 * 用法: ./ws_bench -p 9090 -c 100 -n 1000 -m connect|throughput|echo
 *   -c  并发连接数
 *   -n  总消息数（throughput/echo 模式）
 *   -m  模式: connect(连接容量), throughput(吞吐量), echo(延迟)
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
#include <random>
#include <algorithm>
#include <numeric>
#include <mutex>
#include <cmath>
#include <csignal>

// ==================== 全局统计 ====================

std::atomic<int64_t> g_ws_connected{0};
std::atomic<int64_t> g_ws_failed{0};
std::atomic<int64_t> g_msg_sent{0};
std::atomic<int64_t> g_msg_recv{0};

std::mutex g_lat_mutex;
std::vector<double> g_latencies_us;

// ==================== 工具函数 ====================

inline void set_nb(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

inline int tcp_connect(const char* host, int port) {
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

// 完整 send
bool full_send(int fd, const char* data, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = ::write(fd, data + off, len - off);
        if (n > 0) { off += n; continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            usleep(100);
            continue;
        }
        return false;
    }
    return true;
}

// 完整 recv（读到至少 need 字节，或读到某个 pattern）
std::string full_recv(int fd, int timeout_ms = 5000) {
    std::string result;
    char buf[4096];
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n > 0) {
            result.append(buf, n);
            if (result.find("\r\n\r\n") != std::string::npos) break;
            continue;
        }
        if (n == 0) break;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            usleep(500);
            continue;
        }
        break;
    }
    return result;
}

// ==================== HTTP 注册/登录 ====================

std::string http_post(const char* host, int port, const std::string& path,
                      const std::string& body) {
    int fd = tcp_connect(host, port);
    if (fd < 0) return "";
    set_nb(fd);

    std::ostringstream req;
    req << "POST " << path << " HTTP/1.1\r\n"
        << "Host: " << host << "\r\n"
        << "Content-Type: application/json\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Connection: close\r\n\r\n"
        << body;
    std::string r = req.str();
    full_send(fd, r.data(), r.size());

    std::string resp = full_recv(fd, 5000);
    // Keep reading body
    char buf[4096];
    while (true) {
        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n > 0) { resp.append(buf, n); continue; }
        if (n == 0) break;
        if (errno == EAGAIN || errno == EWOULDBLOCK) { usleep(500); continue; }
        break;
    }
    close(fd);

    auto hdr_end = resp.find("\r\n\r\n");
    if (hdr_end == std::string::npos) return "";
    return resp.substr(hdr_end + 4);
}

// register_and_login() — 自动注册并登录拿 token
std::string register_and_login(const char* host, int port, const std::string& user,
                               const std::string& pass) {
    std::string body = "{\"username\":\"" + user + "\",\"password\":\"" + pass + "\"}";
    http_post(host, port, "/api/register", body); // 注册（已存在也没关系
    std::string resp = http_post(host, port, "/api/login", body);  //登录

    auto pos = resp.find("\"token\":\"");
    if (pos == std::string::npos) return "";
    pos += 9;
    auto end = resp.find("\"", pos);
    if (end == std::string::npos) return "";
    return resp.substr(pos, end - pos); // 返回 token 供 WS 握手用
}

// ==================== WebSocket 帧操作 ====================

// 客户端发送需要 mask -- 编码客户端发送帧
/*
    字节0:  [FIN=1][RSV=000][opcode=0001]  → 0x81 表示"最后一帧文本"
    字节1:  [MASK=1][payload长度]
    字节2-N: 扩展长度（可能没有）
    接下来4字节: mask 密钥
    最后: 加密后的 payload
*/
std::string ws_encode_masked(uint8_t opcode, const std::string& payload) {
    std::string frame;
    frame += (char)(0x80 | opcode);

    uint8_t mask_bit = 0x80;
    // 第二个字节：MASK位 + 长度
    /*
        长度有三种编码方式，WebSocket 协议规定：
            小于 126：直接写
            126~65535：用 126 做标记，后面跟 2 字节
            更大：用 127 做标记，后面跟 8 字节
    */
    if (payload.size() < 126) {
        frame += (char)(mask_bit | payload.size()); //直接塞长度
    } else if (payload.size() <= 65535) {
        frame += (char)(mask_bit | 126); // 126 是标记，意思是"后面2字节才是真实长度
        frame += (char)((payload.size() >> 8) & 0xFF);
        frame += (char)(payload.size() & 0xFF);
    } else {
        frame += (char)(mask_bit | 127); // 127 是标记，意思是"后面8字节才是真实长度
        for (int i = 7; i >= 0; i--)
            frame += (char)((payload.size() >> (8 * i)) & 0xFF);
    }
    /*
        RFC 6455 强制规定客户端发的帧必须 mask，服务端发的不用。mask 就是简单的循环 XOR，密钥 4 字节循环使用。
    */
    static thread_local std::mt19937 rng(std::random_device{}());
    uint32_t mask = rng();  // 随机生成 4 字节密钥
    uint8_t mask_key[4];
    memcpy(mask_key, &mask, 4);
    frame.append((char *)mask_key, 4); // 密钥写入帧

    // 对每个字节做 XOR 加密
    for (size_t i = 0; i < payload.size(); i++)
        frame += (char)(payload[i] ^ mask_key[i % 4]);

    return frame;
}

struct WSFrameResult {
    bool valid = false;
    uint8_t opcode = 0;
    std::string payload;
    size_t consumed = 0;
};

//解析收到的帧
/*
    处理 3 种长度格式（7位/16位/64位），返回 opcode + payload
*/
WSFrameResult ws_try_parse(const std::string& buf, size_t offset = 0) {
    WSFrameResult r;
    size_t pos = offset;
    if (buf.size() - pos < 2) return r;

    uint8_t b0 = (uint8_t)buf[pos++];
    uint8_t b1 = (uint8_t)buf[pos++];
    r.opcode = b0 & 0x0F; // 低4位是操作码
    bool masked = (b1 & 0x80) != 0; // 最高位是 MASK 标记
    uint64_t payload_len = b1 & 0x7F; // 低7位是长度（或长度类型）

    // 解析扩展长度（和 encode 对称）
    if (payload_len == 126) {
        if (buf.size() - pos < 2) return r;
        payload_len = ((uint64_t)(uint8_t)buf[pos] << 8) | (uint8_t)buf[pos+1]; //读2字节
        pos += 2;
    } else if (payload_len == 127) {
        // 读8字节
        if (buf.size() - pos < 8) return r;
        payload_len = 0;
        for (int i = 0; i < 8; i++)
            payload_len = (payload_len << 8) | (uint8_t)buf[pos + i];
        pos += 8;
    }

    uint8_t mask_key[4] = {};
    // 读 mask 密钥（服务端发的帧通常没有 mask）
    if (masked) {
        if (buf.size() - pos < 4) return r;
        memcpy(mask_key, buf.data() + pos, 4);
        pos += 4;
    }

    if (buf.size() - pos < payload_len) return r;

    // 读 payload，如果有 mask 就解密
    r.payload.assign(buf.data() + pos, payload_len);
    if (masked) {
        for (size_t i = 0; i < r.payload.size(); i++)
            r.payload[i] ^= mask_key[i % 4]; // 还原：XOR 两次等于原值
    }
    pos += payload_len;
    r.consumed = pos - offset; // 记录消耗了多少字节，供调用方移动缓冲区指针
    r.valid = true;
    return r;
}

// ==================== WebSocket 连接 ====================

struct WSConn {
    int fd = -1;
    std::string token;
    std::string username;
    std::string recv_buf;
};

// ws_handshake() — 建立 WebSocket 连接
/*
    发 HTTP Upgrade 请求（带 Sec-WebSocket-Key）
    等服务端返回 101 Switching Protocols
    握手后可能跟着历史消息帧，存入 recv_buf

*/
bool ws_handshake(WSConn& c, const char* host, int port) {
    c.fd = tcp_connect(host, port);
    if (c.fd < 0) return false;
    set_nb(c.fd);

    std::string key = "dGhlIHNhbXBsZSBub25jZQ==";
    std::ostringstream req;
    req << "GET /ws?token=" << c.token << " HTTP/1.1\r\n"
        << "Host: " << host << ":" << port << "\r\n"
        << "Upgrade: websocket\r\n"
        << "Connection: Upgrade\r\n"
        << "Sec-WebSocket-Key: " << key << "\r\n"
        << "Sec-WebSocket-Version: 13\r\n"
        << "\r\n";
    std::string r = req.str();
    if (!full_send(c.fd, r.data(), r.size())) {
        close(c.fd); c.fd = -1;
        return false;
    }

    std::string resp = full_recv(c.fd, 5000);
    if (resp.find("101") == std::string::npos) {
        close(c.fd); c.fd = -1;
        return false;
    }

    // 握手后可能有历史消息帧跟着
    auto hdr_end = resp.find("\r\n\r\n");
    if (hdr_end != std::string::npos && hdr_end + 4 < resp.size()) {
        c.recv_buf = resp.substr(hdr_end + 4);
    }

    return true;
}

// 消耗 recv_buf 中已有的所有帧
/*
    把 recv_buf 里已有的帧全部解析出来，防止缓冲区积压撑爆
*/
int drain_frames(WSConn& c) {
    int count = 0;
    size_t offset = 0;
    while (offset < c.recv_buf.size()) {
        auto fr = ws_try_parse(c.recv_buf, offset);
        if (!fr.valid) break;
        offset += fr.consumed;
        if (fr.opcode == 0x1 || fr.opcode == 0x2) count++;
    }
    if (offset > 0) c.recv_buf.erase(0, offset);
    return count;
}

// 非阻塞读取新数据并解析帧
int recv_frames(WSConn& c, int timeout_ms = 100) {
    char buf[8192];
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    int total = 0;

    while (std::chrono::steady_clock::now() < deadline) {
        ssize_t n = ::read(c.fd, buf, sizeof(buf));
        if (n > 0) {
            c.recv_buf.append(buf, n);
            total += drain_frames(c);
            continue;
        }
        if (n == 0) break;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            total += drain_frames(c);
            if (total > 0) break;
            usleep(200);
            continue;
        }
        break;
    }
    total += drain_frames(c);
    return total;
}

// ==================== 测试模式 ====================

// 模式 1: 连接容量测试 - 建立 N 个 WebSocket 连接
void test_connect(const char* host, int port, int num_conns) {
    std::cout << "\n[连接容量测试] 目标: " << num_conns << " 个 WebSocket 连接\n" << std::endl;

    std::vector<WSConn> conns(num_conns);
    auto t0 = std::chrono::high_resolution_clock::now();

    // 先批量注册/登录
    std::cout << "  注册/登录用户..." << std::flush;
    int login_ok = 0;
    for (int i = 0; i < num_conns; i++) {
        std::string user = "wsbench_" + std::to_string(i);
        conns[i].username = user;
        conns[i].token = register_and_login(host, port, user, "pass123");
        if (!conns[i].token.empty()) login_ok++;
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    double login_sec = std::chrono::duration<double>(t1 - t0).count();
    std::cout << " " << login_ok << "/" << num_conns << " (" 
              << std::fixed << std::setprecision(1) << login_sec << "s)" << std::endl;

    // 批量建立 WebSocket
    std::cout << "  建立 WebSocket 连接..." << std::flush;
    auto t2 = std::chrono::high_resolution_clock::now();
    int ws_ok = 0;
    for (int i = 0; i < num_conns; i++) {
        if (conns[i].token.empty()) continue;
        if (ws_handshake(conns[i], host, port)) {
            drain_frames(conns[i]);
            ws_ok++;
        }
        if ((i + 1) % 100 == 0) std::cout << "." << std::flush;
    }
    auto t3 = std::chrono::high_resolution_clock::now();
    double ws_sec = std::chrono::duration<double>(t3 - t2).count();
    std::cout << " " << ws_ok << "/" << num_conns << " (" 
              << std::fixed << std::setprecision(1) << ws_sec << "s)" << std::endl;

    double conn_rate = ws_ok / ws_sec;
    std::cout << "  连接速率: " << std::fixed << std::setprecision(0) 
              << conn_rate << " conn/s" << std::endl;

    // 保持连接一段时间后关闭
    std::cout << "  所有连接已建立，保持 2s..." << std::endl;
    usleep(2000000);

    // 关闭
    for (auto& c : conns) {
        if (c.fd >= 0) { close(c.fd); c.fd = -1; }
    }

    std::cout << "\n  结果:" << std::endl;
    std::cout << "    成功连接: " << ws_ok << std::endl;
    std::cout << "    连接速率: " << std::fixed << std::setprecision(0) << conn_rate << " conn/s" << std::endl;
    std::cout << "    总耗时:   " << std::fixed << std::setprecision(2) 
              << std::chrono::duration<double>(t3 - t0).count() << " s" << std::endl;
}

// 模式 2: 消息吞吐量 - N 个连接各发 M 条消息，测量广播吞吐
void test_throughput(const char* host, int port, int num_conns, int msgs_per_conn,
                     int num_threads) {
    int total_msgs = num_conns * msgs_per_conn;
    std::cout << "\n[消息吞吐量测试] " << num_conns << " 连接 x " << msgs_per_conn
              << " 条 = " << total_msgs << " 消息" << std::endl;
    std::cout << "  (每条消息广播给所有连接，预期接收: " << (int64_t)total_msgs * num_conns << ")" 
              << std::endl;

    // 注册/登录
    std::vector<WSConn> conns(num_conns);
    std::cout << "\n  注册/登录..." << std::flush;
    for (int i = 0; i < num_conns; i++) {
        std::string user = "wstp_" + std::to_string(i);
        conns[i].username = user;
        conns[i].token = register_and_login(host, port, user, "pass123");
    }
    std::cout << " done" << std::endl;

    // 建立 WebSocket
    std::cout << "  建立 WebSocket..." << std::flush;
    int ws_ok = 0;
    for (int i = 0; i < num_conns; i++) {
        if (conns[i].token.empty()) continue;
        if (ws_handshake(conns[i], host, port)) {
            drain_frames(conns[i]);
            ws_ok++;
        }
    }
    std::cout << " " << ws_ok << " 连接" << std::endl;
    if (ws_ok == 0) { std::cerr << "  没有可用连接！" << std::endl; return; }

    // 同时启动发送线程和接收线程，避免缓冲区堵塞
    g_msg_sent.store(0); g_msg_recv.store(0);
    std::atomic<int64_t> total_recv{0};
    std::atomic<bool> send_done{false};
    std::atomic<int> next_conn{0};
    auto t0 = std::chrono::high_resolution_clock::now();

    // 接收线程：持续消费所有连接的广播帧
    std::vector<std::thread> receivers;
    for (int t = 0; t < num_threads; t++) {
        receivers.emplace_back([&, t]() {
            int start = t * ws_ok / num_threads;
            int end = (t + 1) * ws_ok / num_threads;
            while (!send_done.load(std::memory_order_relaxed)) {
                for (int ci = start; ci < end; ci++) {
                    int got = recv_frames(conns[ci], 10);
                    if (got > 0) total_recv.fetch_add(got, std::memory_order_relaxed);
                }
            }
            // 继续读一小段时间清空残余
            for (int round = 0; round < 20; round++) {
                for (int ci = start; ci < end; ci++) {
                    int got = recv_frames(conns[ci], 50);
                    if (got > 0) total_recv.fetch_add(got, std::memory_order_relaxed);
                }
            }
        });
    }

    // 发送线程
    std::vector<std::thread> senders;
    int send_threads = std::min(num_threads, ws_ok);
    for (int t = 0; t < send_threads; t++) {
        senders.emplace_back([&]() {
            while (true) {
                int ci = next_conn.fetch_add(1);
                if (ci >= ws_ok) break;
                WSConn& c = conns[ci];
                for (int m = 0; m < msgs_per_conn; m++) {
                    std::string text = "msg_" + std::to_string(ci) + "_" + std::to_string(m);
                    std::string frame = ws_encode_masked(0x1, text);
                    if (full_send(c.fd, frame.data(), frame.size())) {
                        g_msg_sent.fetch_add(1, std::memory_order_relaxed);
                    }
                    usleep(100);  // 小间隔让接收线程有时间消费
                }
            }
        });
    }

    for (auto& s : senders) s.join();
    auto t_send_done = std::chrono::high_resolution_clock::now();
    double send_sec = std::chrono::duration<double>(t_send_done - t0).count();

    int64_t expected_recv = (int64_t)g_msg_sent.load() * ws_ok;
    std::cout << "  发送完成: " << g_msg_sent.load() << " 条, 耗时 "
              << std::fixed << std::setprecision(2) << send_sec << "s" << std::endl;
    std::cout << "  等待接收完成..." << std::flush;

    // 等接收线程再跑一会儿
    usleep(1000000);
    send_done.store(true);
    for (auto& r : receivers) r.join();

    auto t_recv_done = std::chrono::high_resolution_clock::now();
    double total_sec = std::chrono::duration<double>(t_recv_done - t0).count();

    std::cout << " " << total_recv.load() << " 条" << std::endl;

    // 清理
    for (auto& c : conns) { if (c.fd >= 0) close(c.fd); }

    double send_qps = g_msg_sent.load() / send_sec;
    double recv_qps = total_recv.load() / total_sec;

    std::cout << "\n  结果:" << std::endl;
    std::cout << "    发送:     " << g_msg_sent.load() << " 条" << std::endl;
    std::cout << "    接收:     " << total_recv.load() << " / " << expected_recv
              << " (预期)" << std::endl;
    std::cout << "    发送 QPS: " << std::fixed << std::setprecision(0) << send_qps << " msg/s" << std::endl;
    std::cout << "    广播 QPS: " << std::fixed << std::setprecision(0) << recv_qps << " msg/s (含广播扇出)" << std::endl;
    std::cout << "    总耗时:   " << std::fixed << std::setprecision(2) << total_sec << " s" << std::endl;
}

// 模式 3: 回声延迟 - 多连接，用第一个发消息，测量广播回声延迟
// 后台线程消费其他连接的消息防止缓冲区堵塞
void test_echo_latency(const char* host, int port, int num_conns, int total_msgs) {
    std::cout << "\n[回声延迟测试] " << num_conns << " 连接, " << total_msgs
              << " 条消息(顺序)" << std::endl;

    std::vector<WSConn> conns(num_conns);
    for (int i = 0; i < num_conns; i++) {
        std::string user = "wslat_" + std::to_string(i);
        conns[i].username = user;
        conns[i].token = register_and_login(host, port, user, "pass123");
        if (!conns[i].token.empty()) {
            ws_handshake(conns[i], host, port);
            drain_frames(conns[i]);
        }
    }

    int ws_ok = 0;
    for (auto& c : conns) if (c.fd >= 0) ws_ok++;
    std::cout << "  " << ws_ok << " 个 WebSocket 连接就绪" << std::endl;
    if (ws_ok == 0) { std::cerr << "  没有可用连接！" << std::endl; return; }

    // 后台线程：持续消费 conn[1..N-1] 的广播消息，防止缓冲区满
    std::atomic<bool> drain_stop{false};
    std::atomic<int64_t> drain_count{0};
    std::vector<std::thread> drainers;
    for (int i = 1; i < ws_ok; i++) {
        drainers.emplace_back([&, i]() {
            while (!drain_stop.load(std::memory_order_relaxed)) {
                int n = recv_frames(conns[i], 50);
                if (n > 0) drain_count.fetch_add(n, std::memory_order_relaxed);
            }
            int n = recv_frames(conns[i], 200);
            if (n > 0) drain_count.fetch_add(n, std::memory_order_relaxed);
        });
    }

    // 用 conn[0] 发消息并测延迟
    std::vector<double> latencies;
    latencies.reserve(total_msgs);
    WSConn& sender = conns[0];
    int success = 0, fail = 0;

    std::cout << "  发送 " << total_msgs << " 条并测延迟..." << std::flush;
    for (int i = 0; i < total_msgs; i++) {
        std::string text = "lat_" + std::to_string(i);
        std::string frame = ws_encode_masked(0x1, text);

        auto t0 = std::chrono::high_resolution_clock::now();
        if (!full_send(sender.fd, frame.data(), frame.size())) { fail++; continue; }

        int got = recv_frames(sender, 2000);
        auto t1 = std::chrono::high_resolution_clock::now();

        if (got > 0) {
            double us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
            latencies.push_back(us);
            success++;
        } else {
            fail++;
        }
    }
    std::cout << " done" << std::endl;

    drain_stop.store(true);
    for (auto& t : drainers) t.join();
    std::cout << "  后台连接共消费 " << drain_count.load() << " 条广播\n" << std::endl;

    for (auto& c : conns) { if (c.fd >= 0) close(c.fd); }

    if (latencies.empty()) { std::cout << "  无有效延迟数据！" << std::endl; return; }

    std::sort(latencies.begin(), latencies.end());
    double sum = std::accumulate(latencies.begin(), latencies.end(), 0.0);
    double avg = sum / latencies.size();
    size_t sz = latencies.size();

    std::cout << "  结果 (" << success << " 成功, " << fail << " 失败):" << std::endl;
    std::cout << "    Min:  " << std::fixed << std::setprecision(0) << latencies.front() << " us ("
              << std::setprecision(2) << latencies.front()/1000.0 << " ms)" << std::endl;
    std::cout << "    Avg:  " << std::setprecision(0) << avg << " us ("
              << std::setprecision(2) << avg/1000.0 << " ms)" << std::endl;
    std::cout << "    P50:  " << std::setprecision(0) << latencies[sz*50/100] << " us ("
              << std::setprecision(2) << latencies[sz*50/100]/1000.0 << " ms)" << std::endl;
    std::cout << "    P90:  " << std::setprecision(0) << latencies[sz*90/100] << " us ("
              << std::setprecision(2) << latencies[sz*90/100]/1000.0 << " ms)" << std::endl;
    std::cout << "    P95:  " << std::setprecision(0) << latencies[sz*95/100] << " us ("
              << std::setprecision(2) << latencies[sz*95/100]/1000.0 << " ms)" << std::endl;
    std::cout << "    P99:  " << std::setprecision(0) << latencies[sz*99/100] << " us ("
              << std::setprecision(2) << latencies[sz*99/100]/1000.0 << " ms)" << std::endl;
    std::cout << "    Max:  " << std::setprecision(0) << latencies.back() << " us ("
              << std::setprecision(2) << latencies.back()/1000.0 << " ms)" << std::endl;
}

// 模式 4: 并发回声 - 多连接同时发，每个等自己的回声，多线程
void test_concurrent_echo(const char* host, int port, int num_conns, int msgs_per_conn,
                          int num_threads) {
    int total_msgs = num_conns * msgs_per_conn;
    std::cout << "\n[并发回声延迟测试] " << num_conns << " 连接 x " << msgs_per_conn
              << " 条 = " << total_msgs << " 消息, " << num_threads << " 线程" << std::endl;

    std::vector<WSConn> conns(num_conns);
    std::cout << "  注册/登录 + WebSocket..." << std::flush;
    int ws_ok = 0;
    for (int i = 0; i < num_conns; i++) {
        std::string user = "wsce_" + std::to_string(i);
        conns[i].username = user;
        conns[i].token = register_and_login(host, port, user, "pass123");
        if (!conns[i].token.empty() && ws_handshake(conns[i], host, port)) {
            drain_frames(conns[i]);
            ws_ok++;
        }
    }
    std::cout << " " << ws_ok << " 连接" << std::endl;
    if (ws_ok == 0) { std::cerr << "  没有可用连接！" << std::endl; return; }

    g_latencies_us.clear();
    std::atomic<int> next_conn{0};
    std::atomic<int64_t> total_success{0}, total_fail{0};

    auto t0 = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> threads;
    for (int t = 0; t < num_threads; t++) {
        threads.emplace_back([&]() {
            std::vector<double> local_lats;
            while (true) {
                int ci = next_conn.fetch_add(1);
                if (ci >= ws_ok) break;
                WSConn& c = conns[ci];

                for (int m = 0; m < msgs_per_conn; m++) {
                    std::string text = "ce_" + std::to_string(ci) + "_" + std::to_string(m);
                    std::string frame = ws_encode_masked(0x1, text);

                    auto ts = std::chrono::high_resolution_clock::now();
                    if (!full_send(c.fd, frame.data(), frame.size())) {
                        total_fail.fetch_add(1);
                        continue;
                    }

                    // 等至少收到一帧（可能是自己+别人的广播混在一起）
                    int got = recv_frames(c, 3000);
                    auto te = std::chrono::high_resolution_clock::now();

                    if (got > 0) {
                        double us = std::chrono::duration_cast<std::chrono::microseconds>(te - ts).count();
                        local_lats.push_back(us);
                        total_success.fetch_add(1);
                        // 多余的广播帧也算收到了
                        if (got > 1) g_msg_recv.fetch_add(got - 1);
                    } else {
                        total_fail.fetch_add(1);
                    }
                }
            }
            {
                std::lock_guard<std::mutex> lock(g_lat_mutex);
                g_latencies_us.insert(g_latencies_us.end(), local_lats.begin(), local_lats.end());
            }
        });
    }
    for (auto& t : threads) t.join();

    auto t1 = std::chrono::high_resolution_clock::now();
    double dur = std::chrono::duration<double>(t1 - t0).count();

    for (auto& c : conns) { if (c.fd >= 0) close(c.fd); }

    // 统计延迟
    auto& lats = g_latencies_us;
    if (lats.empty()) {
        std::cout << "  无有效数据！" << std::endl;
        return;
    }

    std::sort(lats.begin(), lats.end());
    double sum = std::accumulate(lats.begin(), lats.end(), 0.0);
    double avg = sum / lats.size();
    size_t sz = lats.size();

    int64_t ok = total_success.load(), fl = total_fail.load();
    double qps = ok / dur;

    std::cout << "\n  结果 (" << ok << " 成功, " << fl << " 失败, "
              << std::fixed << std::setprecision(2) << dur << "s):" << std::endl;
    std::cout << "    QPS:  " << std::setprecision(0) << qps << " echo/s" << std::endl;
    std::cout << "    Avg:  " << std::setprecision(0) << avg << " us ("
              << std::setprecision(2) << avg/1000.0 << " ms)" << std::endl;
    std::cout << "    P50:  " << std::setprecision(0) << lats[sz*50/100] << " us ("
              << std::setprecision(2) << lats[sz*50/100]/1000.0 << " ms)" << std::endl;
    std::cout << "    P90:  " << std::setprecision(0) << lats[sz*90/100] << " us ("
              << std::setprecision(2) << lats[sz*90/100]/1000.0 << " ms)" << std::endl;
    std::cout << "    P95:  " << std::setprecision(0) << lats[sz*95/100] << " us ("
              << std::setprecision(2) << lats[sz*95/100]/1000.0 << " ms)" << std::endl;
    std::cout << "    P99:  " << std::setprecision(0) << lats[sz*99/100] << " us ("
              << std::setprecision(2) << lats[sz*99/100]/1000.0 << " ms)" << std::endl;
    std::cout << "    Max:  " << std::setprecision(0) << lats.back() << " us ("
              << std::setprecision(2) << lats.back()/1000.0 << " ms)" << std::endl;
}

// ==================== main ====================

int main(int argc, char* argv[]) {
    signal(SIGPIPE, SIG_IGN);

    const char* host = "127.0.0.1";
    int port = 9090;
    int num_conns = 100;
    int num_msgs = 1000;
    int num_threads = 4;
    std::string mode = "connect";

    int opt;
    while ((opt = getopt(argc, argv, "h:p:c:n:t:m:")) != -1) {
        switch (opt) {
            case 'h': host = optarg; break;
            case 'p': port = std::stoi(optarg); break;
            case 'c': num_conns = std::stoi(optarg); break;
            case 'n': num_msgs = std::stoi(optarg); break;
            case 't': num_threads = std::stoi(optarg); break;
            case 'm': mode = optarg; break;
        }
    }

    std::cout << "========================================" << std::endl;
    std::cout << "WebSocket Benchmark" << std::endl;
    std::cout << "  Mode:     " << mode << std::endl;
    std::cout << "  Target:   " << host << ":" << port << std::endl;
    std::cout << "  Conns:    " << num_conns << std::endl;
    std::cout << "  Messages: " << num_msgs << std::endl;
    std::cout << "  Threads:  " << num_threads << std::endl;
    std::cout << "========================================" << std::endl;

    if (mode == "connect") {
        test_connect(host, port, num_conns);
    } else if (mode == "throughput") {
        int per = num_msgs / num_conns;
        if (per < 1) per = 1;
        test_throughput(host, port, num_conns, per, num_threads);
    } else if (mode == "echo") {
        test_echo_latency(host, port, num_conns, num_msgs);
    } else if (mode == "concurrent") {
        int per = num_msgs / num_conns;
        if (per < 1) per = 1;
        test_concurrent_echo(host, port, num_conns, per, num_threads);
    } else {
        std::cerr << "未知模式: " << mode << std::endl;
        std::cerr << "可用: connect, throughput, echo, concurrent" << std::endl;
        return 1;
    }

    std::cout << "\n========================================" << std::endl;
    return 0;
}
