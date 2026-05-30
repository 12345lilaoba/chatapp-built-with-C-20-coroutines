/**
 * chat_app.h - 聊天应用业务逻辑层
 *
 * 本文件是整个聊天应用的核心，负责将各个基础设施组件（协程网络库、Redis、MySQL、
 * HTTP 服务器、WebSocket）串联起来，实现完整的聊天业务流程。
 *
 * 整体架构：
 *   ┌─────────┐    HTTP/WS     ┌──────────────┐
 *   │  浏览器  │ ◄──────────► │ handle_http   │ (本文件核心协程)
 *   └─────────┘               │ _connection() │
 *                              └──────┬───────┘
 *                                     │
 *                    ┌────────────────┼────────────────┐
 *                    ▼                ▼                ▼
 *              ┌──────────┐   ┌──────────────┐  ┌──────────┐
 *              │  Redis   │   │    MySQL     │  │ WS管理器  │
 *              │ (token/  │   │  (用户表)    │  │ (广播)    │
 *              │  消息)   │   │              │  │           │
 *              └──────────┘   └──────────────┘  └──────────┘
 *
 * 主要功能模块：
 *   1. 工具函数 - token 生成、密码哈希、时间格式化
 *   2. HTTP 路由 - 静态页面 / 注册 / 登录 / 消息收发 API
 *   3. WebSocket - 握手升级、实时消息循环、历史消息推送
 *   4. Keep-Alive - HTTP/1.1 持久连接支持
 *
 * 依赖关系：
 *   - coro_net.h    : 协程基础设施（Worker、AsyncRead/Write、SwitchToWorker 等）
 *   - async_redis.h : Redis 异步命令封装
 *   - async_mysql.h : MySQL 异步查询封装（通过线程池）
 *   - http_server.h : HTTP 请求/响应解析与序列化
 *   - websocket.h   : WebSocket 帧编解码、连接管理器
 *   - chat_page.h   : 嵌入式 HTML 前端页面（从本文件分离出去）
 */
#pragma once

#include "coro_net.h"
#include "async_redis.h"
#include "async_mysql.h"
#include "http_server.h"
#include "websocket.h"
#include "chat_page.h"
#include <random>
#include <sstream>
#include <ctime>

// ==================== 工具函数 ====================

/**
 * generate_token - 生成 32 位随机字符串作为用户登录 token
 *
 * 使用 thread_local 的 mt19937 随机数生成器，每个线程独立种子，
 * 从 a-z 和 0-9 的字符集中随机抽取 32 个字符拼接而成。
 *
 * 用途：用户登录成功后生成 token，存入 Redis（key: "token:<token>"，value: 用户名），
 *       后续请求通过携带 token 来验证身份。
 *
 * 安全性说明：仅用于演示，生产环境应使用加密安全的随机数生成器（如 /dev/urandom）。
 */
inline std::string generate_token() {
    static const char charset[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    static thread_local std::mt19937 rng(std::random_device{}());
    std::string token;
    token.reserve(32);
    for (int i = 0; i < 32; ++i)
        token += charset[rng() % (sizeof(charset) - 1)];
    return token;
}

/**
 * simple_hash - FNV-1a 哈希算法的简化实现
 *
 * FNV（Fowler-Noll-Vo）是一种非加密哈希函数，特点是实现简单、分布均匀。
 * 初始值 14695981039346656037 和乘数 1099511628211 是 FNV-1a 64 位版本的标准常量。
 *
 * 算法步骤：对每个字节先异或再乘（FNV-1a 顺序），与 FNV-1 的先乘再异或不同，
 * FNV-1a 在雪崩效应上表现更好。
 *
 * 安全性说明：不做加盐处理，不适合生产环境的密码存储，仅作为教学演示。
 */
inline uint64_t simple_hash(const std::string& s) {
    uint64_t h = 14695981039346656037ULL;   // FNV offset basis
    for (char c : s) {
        h ^= (uint64_t)c;                  // 先异或
        h *= 1099511628211ULL;              // 再乘以 FNV prime
    }
    return h;
}

/**
 * hash_password - 将密码转换为 16 位十六进制字符串
 *
 * 调用 simple_hash 得到 64 位哈希值，再格式化为定长 16 字符的十六进制字符串，
 * 用于存入 MySQL 的 password_hash 字段。
 */
inline std::string hash_password(const std::string& password) {
    uint64_t h = simple_hash(password);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016lx", h);
    return std::string(buf);
}

/**
 * current_time_str - 获取当前时间的 "HH:MM:SS" 格式字符串
 *
 * 用于给聊天消息打上时间戳，显示在前端消息气泡的右下角。
 * 使用 localtime_r（线程安全版本）将 time_t 转换为本地时间。
 */
inline std::string current_time_str() {
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", &t);
    return std::string(buf);
}

// ==================== 应用上下文 ====================

/**
 * AppContext - 应用运行时所需的全局资源集合
 *
 * 聚合了各个基础设施组件的指针，方便在不同业务函数间传递。
 * 每个字段的作用：
 *   - redis       : Redis 连接封装，用于 token 存取和聊天消息持久化
 *   - thread_pool : 线程池，MySQL 查询通过线程池异步执行（因为 MySQL C API 是阻塞的）
 *   - mysql_pool  : MySQL 连接池，管理多个数据库连接的复用
 *   - worker      : 协程调度的 Worker，管理 epoll 事件循环和协程恢复
 */
struct AppContext {
    WorkerRedis* redis;
    ThreadPool* thread_pool;
    MySQLPool* mysql_pool;
    Worker* worker;
};

// ==================== HTTP 连接处理协程 ====================

/**
 * handle_http_connection - 处理单个 HTTP 客户端连接的核心协程
 *
 * 这是整个应用最重要的函数，每当 accept() 接收到一个新的客户端连接时，
 * 就会启动一个此协程的实例来处理该连接上的所有请求。
 *
 * 协程执行流程：
 *   1. co_await SwitchToWorker 切换到指定 Worker 的 epoll 上下文
 *   2. 进入 Keep-Alive 循环，在同一 TCP 连接上处理多个 HTTP 请求
 *   3. 读取并解析 HTTP 请求头和请求体
 *   4. 根据请求类型分发：
 *      - WebSocket Upgrade → 进入 WebSocket 消息循环
 *      - 普通 HTTP 请求 → 匹配路由并返回响应
 *   5. 连接关闭时执行清理（从 epoll 移除 fd、关闭 socket）
 *
 * 参数说明：
 *   @param client_fd   : 客户端 socket 文件描述符（已被 accept，已设置非阻塞）
 *   @param worker      : 负责此连接的 Worker（每个 Worker 运行一个 epoll 事件循环）
 *   @param redis       : Redis 连接，用于 token 验证和消息存储
 *   @param thread_pool : 线程池，用于将阻塞的 MySQL 查询异步化
 *   @param mysql_pool  : MySQL 连接池
 *
 * 返回值：FireAndForget 类型的协程，启动后自行运行，调用者无需 co_await。
 *
 * 内存管理：协程帧在协程结束时自动销毁（FireAndForget::promise_type 中的 final_suspend
 *           返回 std::suspend_never，协程完成后自动释放）。
 */

// Minimal JSON string extractor for the flat client messages used by this app.
inline std::string extract_json_field(const std::string& json, const std::string& field) {
    std::string target = "\"" + field + "\":";
    size_t pos = json.find(target);
    if (pos == std::string::npos) {
        target = "\"" + field + "\" :";
        pos = json.find(target);
        if (pos == std::string::npos) return "";
    }
    pos += target.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    if (pos >= json.size() || json[pos] != '"') return "";
    pos++;

    std::string out;
    while (pos < json.size()) {
        char c = json[pos++];
        if (c == '"') break;
        if (c == '\\' && pos < json.size()) {
            out += json[pos++];
        } else {
            out += c;
        }
    }
    return out;
}

inline std::string room_messages_key(const std::string& room_id) {
    return "chatroom:" + room_id + ":messages";
}

inline std::string make_room_deleted_msg(const std::string& room_id) {
    return "{\"action\":\"system\",\"type\":\"room_deleted\",\"room_id\":\""
        + json_escape(room_id) + "\"}";
}

inline FireAndForget handle_http_connection(int client_fd, Worker* worker,
                                            WorkerRedis* redis,
                                            ThreadPool* thread_pool,
                                            MySQLPool* mysql_pool)
{
    // 将协程切换到指定 Worker 的执行上下文中运行
    // SwitchToWorker 会将当前协程的 coroutine_handle 注册到 Worker 的就绪队列
    co_await SwitchToWorker{worker};

    char buf[8192];         // 读取缓冲区，单次最多读 8KB
    std::string leftover;   // 管道化请求的剩余数据：TCP 是字节流，一次 read 可能读到下一个请求的开头

    // ===== Keep-Alive 主循环 =====
    // HTTP/1.1 默认启用 Keep-Alive，同一 TCP 连接可以串行处理多个请求
    // 每次迭代处理一个完整的 HTTP 请求-响应周期
    while (true) {
        // 将上一轮遗留的数据作为本轮起始内容
        std::string raw = std::move(leftover);
        leftover.clear();

        // ----- 阶段 1：读取完整的 HTTP 请求头 -----
        // HTTP 请求头以 "\r\n\r\n" 结尾，循环读取直到找到这个标记
        while (raw.find("\r\n\r\n") == std::string::npos) {
            // AsyncRead: 协程异步读取，内部通过 epoll EPOLLIN 事件触发恢复
            ssize_t n = co_await AsyncRead(client_fd, buf, sizeof(buf) - 1);
            if (n <= 0) {
                // EAGAIN/EWOULDBLOCK: 非阻塞 socket 暂无数据，继续等待
                // EINTR: 被信号中断，重试
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
                    continue;
                // n == 0: 对端关闭连接；其他负值: 真正的错误
                goto cleanup;
            }
            raw.append(buf, n);
        }

        {
            // ----- 阶段 2：解析 HTTP 请求 -----
            HttpRequest req;
            if (!req.parse(raw)) goto cleanup;  // 解析失败，关闭连接

            // 计算完整请求的总长度（请求头 + 请求体）
            size_t hdr_end = raw.find("\r\n\r\n");
            size_t total_header_len = hdr_end + 4;              // +4 是 "\r\n\r\n" 本身的长度
            std::string cl_str = req.header("content-length");
            size_t content_length = cl_str.empty() ? 0 : std::stoul(cl_str);
            size_t total_request_len = total_header_len + content_length;

            // ----- 阶段 3：确保请求体完整接收 -----
            // 对于 POST 请求，Content-Length 指定了请求体的字节数
            // 可能需要多次读取才能收齐
            while (raw.size() < total_request_len) {
                ssize_t n = co_await AsyncRead(client_fd, buf, sizeof(buf) - 1);
                if (n <= 0) {
                    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
                        continue;
                    goto cleanup;
                }
                raw.append(buf, n);
            }

            // ----- 阶段 4：处理管道化（Pipelining）-----
            // 如果读到的数据超过当前请求长度，多余部分属于下一个请求
            if (raw.size() > total_request_len) {
                leftover = raw.substr(total_request_len);
                raw.resize(total_request_len);
            }

            // 用完整数据重新解析（此时 body 已包含在 raw 中）
            req = HttpRequest{};
            if (!req.parse(raw)) goto cleanup;

            // ========== WebSocket 升级检测 ==========
            // 浏览器通过发送带 "Upgrade: websocket" 头的 HTTP 请求来请求协议升级
            std::string upgrade_header = req.header("upgrade");
            if (upgrade_header == "websocket" && req.path == "/ws") {
                // 提取 WebSocket 握手所需的密钥
                std::string ws_key = req.header("sec-websocket-key");
                std::string accept_key = ws_crypto::websocket_accept_key(ws_key);
                std::string token = req.params.count("token") ? req.params["token"] : "";

                std::cerr << "[WS] fd=" << client_fd << " upgrade request, token=" 
                          << token.substr(0, 8) << "..." << std::endl;

                // 通过 Redis 验证 token 对应的用户身份
                auto user_result = co_await AsyncRedisCommand(redis, "GET %s", "token:" + token);
                if (user_result->reply_type != REDIS_REPLY_STRING || user_result->reply_str.empty()) {
                    std::cerr << "[WS] fd=" << client_fd << " token invalid, reject" << std::endl;
                    HttpResponse resp = HttpResponse::error(401, "Unauthorized");
                    std::string data = resp.serialize();
                    size_t off = 0;
                    while (off < data.size()) {
                        ssize_t wn = co_await AsyncWrite(client_fd, data.data() + off, data.size() - off);
                        if (wn <= 0) break;
                        off += wn;
                    }
                    goto cleanup;
                }
                std::string ws_username = user_result->reply_str;
                std::string current_room = req.params.count("room") ? req.params["room"] : "general";
                if (current_room.empty()) current_room = "general";

                if (current_room != "general") {
                    std::string esc_room = mysql_escape(current_room);
                    std::string esc_user = mysql_escape(ws_username);
                    auto member_check = co_await AsyncMySQLQuery(
                        worker, thread_pool, mysql_pool,
                        "SELECT r.room_id FROM rooms r "
                        "JOIN room_members m ON r.room_id=m.room_id "
                        "WHERE r.room_id='" + esc_room + "' AND m.username='" + esc_user + "' LIMIT 1");
                    if (!member_check.success || member_check.rows.empty()) {
                        HttpResponse resp = HttpResponse::error(403, "Forbidden");
                        std::string data = resp.serialize();
                        size_t off = 0;
                        while (off < data.size()) {
                            ssize_t wn = co_await AsyncWrite(client_fd, data.data() + off, data.size() - off);
                            if (wn <= 0) break;
                            off += (size_t)wn;
                        }
                        goto cleanup;
                    }
                }

                std::cerr << "[WS] fd=" << client_fd << " user=" << ws_username
                          << " room=" << current_room << " handshake ok" << std::endl;

                std::string handshake =
                    "HTTP/1.1 101 Switching Protocols\r\n"
                    "Upgrade: websocket\r\n"
                    "Connection: Upgrade\r\n"
                    "Sec-WebSocket-Accept: " + accept_key + "\r\n"
                    "\r\n";
                {
                    size_t off = 0;
                    while (off < handshake.size()) {
                        ssize_t wn = co_await AsyncWrite(client_fd, handshake.data() + off, handshake.size() - off);
                        if (wn <= 0) goto cleanup;
                        off += (size_t)wn;
                    }
                }

                g_ws_manager.add(client_fd, worker, ws_username);
                g_ws_manager.join_room(client_fd, current_room);

                auto send_history = [&](std::string room_id) -> FireAndForget {
                    std::string key = room_messages_key(room_id);
                    auto msgs = co_await AsyncRedisCommand(redis, "LRANGE %s %s %s", key.c_str(), "-50", "-1");
                    if (msgs->reply_type == REDIS_REPLY_ARRAY && !msgs->reply_str.empty()) {
                        std::istringstream iss(msgs->reply_str);
                        std::string line;
                        while (std::getline(iss, line)) {
                            if (line.empty()) continue;
                            size_t p1 = line.find('|');
                            size_t p2 = line.find('|', p1 + 1);
                            if (p1 == std::string::npos || p2 == std::string::npos) continue;
                            std::string u = line.substr(0, p1);
                            std::string t = line.substr(p1 + 1, p2 - p1 - 1);
                            std::string m = line.substr(p2 + 1);
                            std::string json_msg = "{\"room_id\":\"" + json_escape(room_id)
                                + "\",\"user\":\"" + json_escape(u)
                                + "\",\"time\":\"" + json_escape(t)
                                + "\",\"text\":\"" + json_escape(m) + "\"}";
                            std::string frame = ws_encode_frame(WS_TEXT, json_msg);
                            size_t off = 0;
                            while (off < frame.size()) {
                                ssize_t wn = co_await AsyncWrite(client_fd, frame.data() + off, frame.size() - off);
                                if (wn <= 0) co_return;
                                off += (size_t)wn;
                            }
                        }
                    }
                };
                send_history(current_room);

                std::cerr << "[WS] fd=" << client_fd << " entering msg loop" << std::endl;
                while (true) {
                    uint8_t header[2];
                    {
                        ssize_t n;
                        do {
                            n = co_await AsyncRead(client_fd, (char*)header, 2);
                        } while (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR));
                        if (n <= 0) break;
                        if (n == 1) {
                            ssize_t n2;
                            do {
                                n2 = co_await AsyncRead(client_fd, (char*)header + 1, 1);
                            } while (n2 < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR));
                            if (n2 <= 0) break;
                        }
                    }

                    uint8_t opcode = header[0] & 0x0F;
                    bool masked = (header[1] & 0x80) != 0;
                    uint64_t payload_len = header[1] & 0x7F;

                    if (payload_len == 126) {
                        uint8_t ext[2];
                        ssize_t n = co_await AsyncRead(client_fd, (char*)ext, 2);
                        if (n <= 0) break;
                        payload_len = ((uint64_t)ext[0] << 8) | ext[1];
                    } else if (payload_len == 127) {
                        uint8_t ext[8];
                        ssize_t n = co_await AsyncRead(client_fd, (char*)ext, 8);
                        if (n <= 0) break;
                        payload_len = 0;
                        for (int i = 0; i < 8; i++) payload_len = (payload_len << 8) | ext[i];
                    }

                    uint8_t mask_key[4] = {};
                    if (masked) {
                        ssize_t n = co_await AsyncRead(client_fd, (char*)mask_key, 4);
                        if (n <= 0) break;
                    }

                    std::string payload(payload_len, '\0');
                    size_t read_off = 0;
                    while (read_off < payload_len) {
                        ssize_t n = co_await AsyncRead(client_fd, &payload[read_off], payload_len - read_off);
                        if (n <= 0) goto ws_cleanup;
                        read_off += (size_t)n;
                    }

                    if (masked) {
                        for (size_t i = 0; i < payload.size(); i++) payload[i] ^= mask_key[i % 4];
                    }

                    if (opcode == WS_CLOSE) break;
                    if (opcode == WS_PING) {
                        std::string pong = ws_encode_frame(WS_PONG, payload);
                        size_t off = 0;
                        while (off < pong.size()) {
                            ssize_t wn = co_await AsyncWrite(client_fd, pong.data() + off, pong.size() - off);
                            if (wn <= 0) goto ws_cleanup;
                            off += (size_t)wn;
                        }
                        continue;
                    }

                    if (opcode == WS_TEXT && !payload.empty()) {
                        std::string action = extract_json_field(payload, "action");
                        std::string room_id = extract_json_field(payload, "room_id");
                        if (room_id.empty()) room_id = current_room;

                        if (action == "join" && !room_id.empty()) {
                            current_room = room_id;
                            g_ws_manager.join_room(client_fd, room_id);
                            send_history(room_id);
                        } else if (action == "leave" && !room_id.empty()) {
                            g_ws_manager.leave_room(client_fd, room_id);
                        } else {
                            std::string text = action == "send" ? extract_json_field(payload, "text") : payload;
                            if (text.empty()) continue;

                            if (room_id != "general") {
                                std::string esc_room = mysql_escape(room_id);
                                std::string esc_user = mysql_escape(ws_username);
                                auto member_check = co_await AsyncMySQLQuery(
                                    worker, thread_pool, mysql_pool,
                                    "SELECT r.room_id FROM rooms r "
                                    "JOIN room_members m ON r.room_id=m.room_id "
                                    "WHERE r.room_id='" + esc_room + "' AND m.username='" + esc_user + "' LIMIT 1");
                                if (!member_check.success || member_check.rows.empty()) {
                                    g_ws_manager.leave_room(client_fd, room_id);
                                    std::string sys_msg = "{\"action\":\"system\",\"type\":\"room_missing\",\"room_id\":\""
                                        + json_escape(room_id) + "\",\"message\":\"群不存在或你已不在该群，已自动退出。\"}";
                                    std::string frame = ws_encode_frame(WS_TEXT, sys_msg);
                                    size_t off = 0;
                                    while (off < frame.size()) {
                                        ssize_t wn = co_await AsyncWrite(client_fd, frame.data() + off, frame.size() - off);
                                        if (wn <= 0) goto ws_cleanup;
                                        off += (size_t)wn;
                                    }
                                    continue;
                                }
                            }

                            std::string time_str = current_time_str();
                            std::string entry = ws_username + "|" + time_str + "|" + text;
                            std::string redis_key = room_messages_key(room_id);
                            co_await AsyncRedisCommand(redis, "RPUSH %s %s", redis_key.c_str(), entry);
                            co_await AsyncRedisCommand(redis, "LTRIM %s %s %s", redis_key.c_str(), "-200", "-1");

                            std::string json_msg = "{\"room_id\":\"" + json_escape(room_id)
                                + "\",\"user\":\"" + json_escape(ws_username)
                                + "\",\"time\":\"" + json_escape(time_str)
                                + "\",\"text\":\"" + json_escape(text) + "\"}";
                            g_ws_manager.broadcast_to_room(room_id, json_msg);
                        }
                    }
                }


            ws_cleanup:
                std::cerr << "[WS] fd=" << client_fd << " user=" << ws_username 
                          << " disconnected" << std::endl;
                g_ws_manager.remove(client_fd);
                goto cleanup;   // WebSocket 连接结束后直接关闭 TCP
            }

            // ========== HTTP Keep-Alive 判断 ==========
            // HTTP/1.1 默认 Keep-Alive，除非显式带 "Connection: close"
            // HTTP/1.0 默认关闭，除非显式带 "Connection: keep-alive"
            std::string conn_header = req.header("connection");
            bool keep_alive = false;
            if (req.version == "HTTP/1.1") {
                keep_alive = (conn_header.find("close") == std::string::npos);
            } else {
                keep_alive = (conn_header.find("keep-alive") != std::string::npos ||
                              conn_header.find("Keep-Alive") != std::string::npos);
            }

            // ========== 路由分发 ==========
            HttpResponse resp;

            // --- GET / : 返回聊天应用的 HTML 前端页面 ---
            if (req.method == "GET" && req.path == "/") {
                resp = HttpResponse::ok(HTML_PAGE);
            }
            // --- POST /api/register : 用户注册 ---
            // 请求体 JSON: {"username": "xxx", "password": "xxx"}
            // 流程：校验参数 → 哈希密码 → INSERT IGNORE 到 MySQL
            //       affected_rows == 0 表示用户名已存在（UNIQUE 约束）
            else if (req.method == "POST" && req.path == "/api/register") {
                std::string username = req.json_get("username");
                std::string password = req.json_get("password");

                if (username.empty() || password.empty()) {
                    resp = HttpResponse::error(400, "用户名和密码不能为空");
                } else {
                    std::string pwd_hash = hash_password(password);
                    std::string esc_user = mysql_escape(username);
                    std::string esc_pwd = mysql_escape(pwd_hash);

                    auto insert = co_await AsyncMySQLQuery(
                        worker, thread_pool, mysql_pool,
                        "INSERT IGNORE INTO users(username, password_hash) VALUES('"
                        + esc_user + "', '" + esc_pwd + "')");

                    if (!insert.success) {
                        resp = HttpResponse::error(500, "注册失败: " + insert.error);
                    } else if (insert.affected_rows == 0) {
                        resp = HttpResponse::json("{\"error\":\"用户名已存在\"}");
                        resp.status = 409;
                        resp.status_text = "Conflict";
                    } else {
                        resp = HttpResponse::json("{\"message\":\"注册成功\"}");
                    }
                }
            }
            // --- POST /api/login : 用户登录 ---
            // 流程：校验参数 → 哈希密码 → 查询 MySQL 验证
            //       成功后生成 token 存入 Redis（1 小时过期）
            else if (req.method == "POST" && req.path == "/api/login") {
                std::string username = req.json_get("username");
                std::string password = req.json_get("password");

                if (username.empty() || password.empty()) {
                    resp = HttpResponse::error(400, "用户名和密码不能为空");
                } else {
                    std::string pwd_hash = hash_password(password);
                    std::string esc_user = mysql_escape(username);
                    std::string esc_pwd = mysql_escape(pwd_hash);

                    auto result = co_await AsyncMySQLQuery(
                        worker, thread_pool, mysql_pool,
                        "SELECT id FROM users WHERE username='" + esc_user
                        + "' AND password_hash='" + esc_pwd + "' LIMIT 1");

                    if (result.success && !result.rows.empty()) {
                        std::string tok = generate_token();
                        // 将 token 存入 Redis：key="token:<token>", value=用户名
                        co_await AsyncRedisCommand(redis, "SET %s %s", "token:" + tok, username);
                        // 设置 1 小时（3600 秒）过期
                        co_await AsyncRedisCommand(redis, "EXPIRE %s %s", "token:" + tok, "3600");
                        resp = HttpResponse::json("{\"token\":\"" + tok + "\",\"message\":\"登录成功\"}");
                    } else {
                        resp = HttpResponse::error(401, "用户名或密码错误");
                    }
                }
            }

            // --- GET /api/rooms : 获取房间列表和当前用户加入状态 ---
            else if (req.method == "GET" && req.path == "/api/rooms") {
                std::string tok = req.params.count("token") ? req.params["token"] : "";
                std::string username;
                if (!tok.empty()) {
                    auto user_result = co_await AsyncRedisCommand(redis, "GET %s", "token:" + tok);
                    if (user_result->reply_type == REDIS_REPLY_STRING) username = user_result->reply_str;
                }

                std::string sql =
                    "SELECT r.room_id, r.name, r.description, r.owner_username, "
                    "IF(m.username IS NOT NULL, 1, 0) "
                    "FROM rooms r LEFT JOIN room_members m "
                    "ON r.room_id=m.room_id AND m.username='" + mysql_escape(username) + "' "
                    "ORDER BY r.created_at ASC";
                auto result = co_await AsyncMySQLQuery(worker, thread_pool, mysql_pool, sql);
                if (!result.success) {
                    resp = HttpResponse::error(500, "Database error");
                } else {
                    std::string json = "{\"rooms\":[";
                    for (size_t i = 0; i < result.rows.size(); ++i) {
                        if (i) json += ",";
                        json += "{\"id\":\"" + json_escape(result.rows[i][0]) + "\",";
                        json += "\"name\":\"" + json_escape(result.rows[i][1]) + "\",";
                        json += "\"description\":\"" + json_escape(result.rows[i][2]) + "\",";
                        json += "\"owner\":\"" + json_escape(result.rows[i][3]) + "\",";
                        json += "\"joined\":" + (result.rows[i][4] == "1" || result.rows[i][0] == "general" ? std::string("true") : std::string("false")) + "}";
                    }
                    json += "]}";
                    resp = HttpResponse::json(json);
                }
            }
            // --- POST /api/room/create : 创建房间 ---
            else if (req.method == "POST" && req.path == "/api/room/create") {
                std::string tok = req.json_get("token");
                std::string room_id = req.json_get("room_id");
                std::string name = req.json_get("name");
                std::string desc = req.json_get("description");
                if (tok.empty() || room_id.empty() || name.empty()) {
                    resp = HttpResponse::error(400, "缺少参数");
                } else {
                    auto user_result = co_await AsyncRedisCommand(redis, "GET %s", "token:" + tok);
                    if (user_result->reply_type != REDIS_REPLY_STRING || user_result->reply_str.empty()) {
                        resp = HttpResponse::error(401, "未登录");
                    } else {
                        std::string username = user_result->reply_str;
                        std::string esc_id = mysql_escape(room_id);
                        std::string esc_user = mysql_escape(username);
                        auto result = co_await AsyncMySQLQuery(
                            worker, thread_pool, mysql_pool,
                            "INSERT INTO rooms(room_id,name,description,owner_username) VALUES('"
                            + esc_id + "','" + mysql_escape(name) + "','" + mysql_escape(desc) + "','" + esc_user + "')");
                        if (!result.success) {
                            resp = HttpResponse::error(409, "房间已存在或创建失败");
                        } else {
                            co_await AsyncMySQLQuery(worker, thread_pool, mysql_pool,
                                "INSERT IGNORE INTO room_members(room_id,username) VALUES('" + esc_id + "','" + esc_user + "')");
                            resp = HttpResponse::json("{\"message\":\"房间创建成功\"}");
                        }
                    }
                }
            }
            // --- POST /api/room/join : 加入房间 ---
            else if (req.method == "POST" && req.path == "/api/room/join") {
                std::string tok = req.json_get("token");
                std::string room_id = req.json_get("room_id");
                if (tok.empty() || room_id.empty()) {
                    resp = HttpResponse::error(400, "缺少参数");
                } else {
                    auto user_result = co_await AsyncRedisCommand(redis, "GET %s", "token:" + tok);
                    if (user_result->reply_type != REDIS_REPLY_STRING || user_result->reply_str.empty()) {
                        resp = HttpResponse::error(401, "未登录");
                    } else {
                        std::string esc_id = mysql_escape(room_id);
                        std::string esc_user = mysql_escape(user_result->reply_str);
                        auto exists = co_await AsyncMySQLQuery(worker, thread_pool, mysql_pool,
                            "SELECT room_id FROM rooms WHERE room_id='" + esc_id + "' LIMIT 1");
                        if (!exists.success || exists.rows.empty()) {
                            resp = HttpResponse::error(404, "房间不存在");
                        } else {
                            co_await AsyncMySQLQuery(worker, thread_pool, mysql_pool,
                                "INSERT IGNORE INTO room_members(room_id,username) VALUES('" + esc_id + "','" + esc_user + "')");
                            resp = HttpResponse::json("{\"message\":\"加入成功\"}");
                        }
                    }
                }
            }
            // --- POST /api/room/leave : 退出房间 ---
            else if (req.method == "POST" && req.path == "/api/room/leave") {
                std::string tok = req.json_get("token");
                std::string room_id = req.json_get("room_id");
                if (tok.empty() || room_id.empty()) {
                    resp = HttpResponse::error(400, "缺少参数");
                } else if (room_id == "general") {
                    resp = HttpResponse::error(403, "不能退出 general 房间");
                } else {
                    auto user_result = co_await AsyncRedisCommand(redis, "GET %s", "token:" + tok);
                    if (user_result->reply_type != REDIS_REPLY_STRING || user_result->reply_str.empty()) {
                        resp = HttpResponse::error(401, "未登录");
                    } else {
                        co_await AsyncMySQLQuery(worker, thread_pool, mysql_pool,
                            "DELETE FROM room_members WHERE room_id='" + mysql_escape(room_id)
                            + "' AND username='" + mysql_escape(user_result->reply_str) + "'");
                        resp = HttpResponse::json("{\"message\":\"退出成功\"}");
                    }
                }
            }
            // --- POST /api/room/delete : 删除房间，仅 owner 可删 ---
            else if (req.method == "POST" && req.path == "/api/room/delete") {
                std::string tok = req.json_get("token");
                std::string room_id = req.json_get("room_id");
                if (tok.empty() || room_id.empty()) {
                    resp = HttpResponse::error(400, "缺少参数");
                } else if (room_id == "general") {
                    resp = HttpResponse::error(403, "不能删除 general 房间");
                } else {
                    auto user_result = co_await AsyncRedisCommand(redis, "GET %s", "token:" + tok);
                    if (user_result->reply_type != REDIS_REPLY_STRING || user_result->reply_str.empty()) {
                        resp = HttpResponse::error(401, "未登录");
                    } else {
                        std::string esc_id = mysql_escape(room_id);
                        auto owner = co_await AsyncMySQLQuery(worker, thread_pool, mysql_pool,
                            "SELECT owner_username FROM rooms WHERE room_id='" + esc_id + "' LIMIT 1");
                        if (!owner.success || owner.rows.empty()) {
                            resp = HttpResponse::error(404, "房间不存在");
                        } else if (owner.rows[0][0] != user_result->reply_str) {
                            resp = HttpResponse::error(403, "无权删除");
                        } else {
                            co_await AsyncMySQLQuery(worker, thread_pool, mysql_pool,
                                "DELETE FROM room_members WHERE room_id='" + esc_id + "'");
                            co_await AsyncMySQLQuery(worker, thread_pool, mysql_pool,
                                "DELETE FROM rooms WHERE room_id='" + esc_id + "'");
                            co_await AsyncRedisCommand(redis, "DEL %s", room_messages_key(room_id));
                            g_ws_manager.broadcast_to_room(room_id, make_room_deleted_msg(room_id));
                            g_ws_manager.evict_room(room_id);
                            resp = HttpResponse::json("{\"message\":\"删除成功\"}");
                        }
                    }
                }
            }

            /*
                浏览器或环境不支持 WebSocket
                WebSocket 连接断了
                某些代理/网关不允许 WebSocket
                调试时不想写 WebSocket 客户端
                压测时想只测 HTTP 路径

                以上条件就会用普通的HTTP发消息
            */
            // --- POST /api/send : 通过 HTTP 发送聊天消息（REST 备用接口）---
            // 主要的消息发送走 WebSocket，此接口作为降级方案
            else if (req.method == "POST" && req.path == "/api/send") {
                std::string tok = req.json_get("token");
                std::string message = req.json_get("message");

                if (tok.empty() || message.empty()) {
                    resp = HttpResponse::error(400, "缺少参数");
                } else {
                    auto user_result = co_await AsyncRedisCommand(redis, "GET %s", "token:" + tok);
                    if (user_result->reply_type != REDIS_REPLY_STRING || user_result->reply_str.empty()) {
                        resp = HttpResponse::error(401, "未登录或登录已过期");
                    } else {
                        std::string username = user_result->reply_str;
                        std::string time_str = current_time_str();
                        std::string entry = username + "|" + time_str + "|" + message;
                        co_await AsyncRedisCommand(redis, "RPUSH %s %s", "chatroom:messages", entry);
                        co_await AsyncRedisCommand(redis, "LTRIM %s %s %s", "chatroom:messages", "-200", "-1");
                        resp = HttpResponse::json("{\"message\":\"ok\"}");
                    }
                }
            }
            /*
                REST备用接口 可以让客户端不用 WebSocket 也能拉取历史消息。
                作用包括：
                    页面刚打开时拉历史记录
                    WebSocket 断线重连后补消息
                    非 WebSocket 客户端查看聊天记录
                    调试 Redis 消息存储是否正常
                    HTTP 压测/接口测试
            */
            // --- GET /api/messages : 获取历史聊天消息（REST 备用接口）---
            else if (req.method == "GET" && req.path == "/api/messages") {
                std::string tok = req.params.count("token") ? req.params["token"] : "";

                if (tok.empty()) {
                    resp = HttpResponse::error(401, "未登录");
                } else {
                    auto user_result = co_await AsyncRedisCommand(redis, "GET %s", "token:" + tok);
                    if (user_result->reply_type != REDIS_REPLY_STRING || user_result->reply_str.empty()) {
                        resp = HttpResponse::error(401, "未登录或登录已过期");
                    } else {
                        // 从 Redis list 拉取最近 100 条消息
                        auto msgs = co_await AsyncRedisCommand(redis, "LRANGE %s %s %s",
                                                                "chatroom:messages", "-100", "-1");
                        std::string json = "{\"messages\":[";
                        if (msgs->reply_type == REDIS_REPLY_ARRAY && !msgs->reply_str.empty()) {
                            std::istringstream iss(msgs->reply_str);
                            std::string line;
                            bool first = true;
                            while (std::getline(iss, line)) {
                                if (line.empty()) continue;
                                size_t p1 = line.find('|');
                                size_t p2 = line.find('|', p1 + 1);
                                if (p1 == std::string::npos || p2 == std::string::npos) continue;
                                std::string user = line.substr(0, p1);
                                std::string time = line.substr(p1 + 1, p2 - p1 - 1);
                                std::string text = line.substr(p2 + 1);
                                if (!first) json += ",";
                                first = false;
                                json += "{\"user\":\"" + json_escape(user)
                                      + "\",\"time\":\"" + json_escape(time)
                                      + "\",\"text\":\"" + json_escape(text) + "\"}";
                            }
                        }
                        json += "]}";
                        resp = HttpResponse::json(json);
                    }
                }
            }
            // --- 404 兜底 ---
            else {
                resp = HttpResponse::error(404, "Not Found");
            }

            // ========== 设置连接管理头 ==========
            if (keep_alive) {
                resp.headers["Connection"] = "keep-alive";
                resp.headers["Keep-Alive"] = "timeout=30, max=1000";
            } else {
                resp.headers["Connection"] = "close";
            }

            // ========== 发送 HTTP 响应 ==========
            std::string response_data = resp.serialize();
            size_t off = 0;
            while (off < response_data.size()) {
                ssize_t wn = co_await AsyncWrite(client_fd,
                                                 response_data.data() + off,
                                                 response_data.size() - off);
                /*
                    如果 wn > 0：
                    正常，off += wn，继续写剩余数据

                    如果 wn < 0 且 errno 是 EAGAIN/EWOULDBLOCK/EINTR：
                    这不算真正失败，继续重试

                    其他 wn == 0：
                    认为连接出问题，关闭连接
                */
                if (wn <= 0) {
                    if (wn < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
                        continue;
                    goto cleanup;
                }
                off += (size_t)wn;
            }

            if (!keep_alive) goto cleanup;
        }
    }

// ========== 连接清理 ==========
// 无论是正常关闭还是异常退出，都会执行到这里
cleanup:
    worker->del_client_fd(client_fd);   // 从 epoll 中移除此 fd 的监听
    close(client_fd);                   // 关闭 socket
}
