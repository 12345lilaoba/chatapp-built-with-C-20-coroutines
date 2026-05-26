/**
 * websocket.h - WebSocket 协议实现
 *
 * 提供：
 *   - WebSocket 握手（HTTP Upgrade）
 *   - WebSocket 帧解析/编码（RFC 6455）
 *   - WSManager: 在线连接管理 + 消息广播
 */
#pragma once

#include "coro_net.h"
#include <string>
#include <vector>
#include <map>
#include <set>
#include <mutex>
#include <functional>
#include <cstring>

// ==================== SHA1 + Base64（WebSocket 握手用）====================

namespace ws_crypto {

// 极简 SHA1 实现（仅供 WebSocket 握手使用）
inline void sha1(const uint8_t* data, size_t len, uint8_t hash[20]) {
    // 初始化 5 个哈希状态
    uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE,
             h3 = 0x10325476, h4 = 0xC3D2E1F0;

    // 做消息填充，SHA1 要按 64 字节一块处理。
    size_t new_len = len + 1;
    while (new_len % 64 != 56) new_len++;
    std::vector<uint8_t> msg(new_len + 8, 0);
    memcpy(msg.data(), data, len);
    msg[len] = 0x80;

    // 写入原始 bit 长度，SHA1 最后 8 字节要保存原始消息长度，单位是 bit。
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++)
        msg[new_len + i] = (uint8_t)(bits >> (56 - 8 * i));

    //按 64 字节分块处理，
    for (size_t offset = 0; offset < msg.size(); offset += 64) {
        uint32_t w[80];
        // 生成 80 个 32 位字，先把当前 64 字节拆成 16 个 32 位整数：
        for (int i = 0; i < 16; i++)
            w[i] = ((uint32_t)msg[offset + 4*i] << 24) |
                    ((uint32_t)msg[offset + 4*i+1] << 16) |
                    ((uint32_t)msg[offset + 4*i+2] << 8) |
                    ((uint32_t)msg[offset + 4*i+3]);
        // 然后扩展成 80 个，这就是 SHA1 的消息扩展。
        for (int i = 16; i < 80; i++) {
            uint32_t t = w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16];
            w[i] = (t << 1) | (t >> 31);
        }

        //主循环 80 轮
        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            // 不同轮数使用不同函数和常量：
            if (i < 20)      { f = (b & c) | ((~b) & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else              { f = b ^ c ^ d; k = 0xCA62C1D6; }
            uint32_t temp = ((a << 5) | (a >> 27)) + f + e + k + w[i];
            e = d; d = c; c = (b << 30) | (b >> 2); b = a; a = temp;
        }
        // 当前块处理完后，把结果累加回总体状态
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }

    // 把 5 个 uint32_t 转成 20 字节输出
    uint32_t hh[5] = {h0, h1, h2, h3, h4};
    for (int i = 0; i < 5; i++) {
        hash[4*i]   = (uint8_t)(hh[i] >> 24);
        hash[4*i+1] = (uint8_t)(hh[i] >> 16);
        hash[4*i+2] = (uint8_t)(hh[i] >> 8);
        hash[4*i+3] = (uint8_t)(hh[i]);
    }
}

inline std::string base64_encode(const uint8_t* data, size_t len) {
    //base64表
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve((len + 2) / 3 * 4);
    //每 3 个字节编码成 4 个 Base64 字符：
    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = ((uint32_t)data[i]) << 16;
        if (i + 1 < len) n |= ((uint32_t)data[i+1]) << 8;
        if (i + 2 < len) n |= ((uint32_t)data[i+2]);
        // 然后每 6 bit 取一次：
        result += table[(n >> 18) & 63];
        result += table[(n >> 12) & 63];
        //= 是 Base64 的补位字符。
        result += (i + 1 < len) ? table[(n >> 6) & 63] : '=';
        result += (i + 2 < len) ? table[n & 63] : '=';
    }
    return result;
}

/*
    websocket_accept_key函数就是 WebSocket 握手最关键的函数。

    客户端握手时会发：    Sec-WebSocket-Key: xxxxx
    服务端必须返回：    Sec-WebSocket-Accept: yyyyy
*/

inline std::string websocket_accept_key(const std::string& client_key) {
    // 这个 GUID 是 WebSocket 标准规定的固定字符串。
    std::string combined = client_key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    uint8_t hash[20];
    sha1((const uint8_t*)combined.data(), combined.size(), hash);
    return base64_encode(hash, 20);
}

} // namespace ws_crypto

// ==================== WebSocket 帧 ====================

enum WSOpcode : uint8_t {
    WS_TEXT    = 0x1, //0x1 文本消息
    WS_BINARY  = 0x2, //0x2 二进制消息
    WS_CLOSE   = 0x8, //0x8 关闭连接
    WS_PING    = 0x9, //0x9 ping
    WS_PONG    = 0xA, //0xA pong
};

// WebSocket 帧的结构体，不过当前项目里这个结构体基本没有被实际使用，更多是作为协议模型保留。
struct WSFrame {
    bool fin = true;            //是否是最后一帧
    uint8_t opcode = WS_TEXT;   //帧类型
    std::string payload;        //数据内容
};

// 编码一个 WebSocket 帧（服务器发送，不需要 mask）
inline std::string ws_encode_frame(uint8_t opcode, const std::string& payload) {
    std::string frame;
    frame += (char)(0x80 | opcode);  // FIN + opcode

    /*
        WebSocket 长度有三种编码方式：
            如果payload小于126字节：
                第二字节直接放长度 ->比如 "hi" 长度 2 则81 02 68 69，其中 02 就是长度。
            如果 payload 在 126 到 65535 之间：
                第二字节放 126，后面再用 2 字节表示真实长度。高字节在前，低字节在后，也就是网络字节序/大端序。
            如果 payload 更大：
                第二字节放 127，后面再用 8 字节表示真实长度。
    */
    if (payload.size() < 126) {
        frame += (char)payload.size();
    } else if (payload.size() <= 65535) {
        frame += (char)126;
        frame += (char)((payload.size() >> 8) & 0xFF);
        frame += (char)(payload.size() & 0xFF);
    } else {
        frame += (char)127;
        for (int i = 7; i >= 0; i--)
            frame += (char)((payload.size() >> (8 * i)) & 0xFF);
    }

    //最后把真实消息内容拼到帧后面。
    frame += payload;
    return frame;
}

// ==================== WSManager: 在线连接管理 + 广播 ====================

struct WSClient {
    int fd;
    Worker* worker;
    std::string username;
};

class WSManager {
public:
    void add(int fd, Worker* worker, const std::string& username) {
        std::lock_guard<std::mutex> lock(mu_);
        clients_[fd] = {fd, worker, username};
    }

    void remove(int fd) {
        std::lock_guard<std::mutex> lock(mu_);
        clients_.erase(fd);
    }

    // 广播消息给所有在线客户端
    // WebSocket 帧通常 < 1KB，非阻塞 fd 上同步写入基本不会阻塞
    void broadcast(const std::string& message) {
        std::string frame = ws_encode_frame(WS_TEXT, message);
        // 把 JSON 消息编码成 WebSocket 文本帧。
        std::lock_guard<std::mutex> lock(mu_);
        /*
            循环写，直到这个客户端写完，或者写失败。
            这里没有用 AsyncWrite，而是直接同步 write。因为注释里假设 WebSocket 消息通常小于 1KB，非阻塞 socket 上一般不会阻塞太久。
            但要注意：这是演示级实现。如果客户端很多或者某些客户端很慢，这里可能出现：
                1、写不完整
                2、写失败后消息丢失
                3、持锁期间做 IO，影响其他 add/remove/broadcast
        */
        for (auto& [fd, client] : clients_) {
            size_t off = 0;
            while (off < frame.size()) {
                ssize_t n = ::write(fd, frame.data() + off, frame.size() - off);
                if (n <= 0) break;
                off += n;
            }
        }
    }

    int online_count() {
        std::lock_guard<std::mutex> lock(mu_);
        return (int)clients_.size();
    }

private:
    std::map<int, WSClient> clients_;
    std::mutex mu_;
};

// 全局 WebSocket 管理器
inline WSManager g_ws_manager;
