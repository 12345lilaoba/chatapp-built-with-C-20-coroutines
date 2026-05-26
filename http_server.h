/**
 * http_server.h - 简单 HTTP 解析和路由
 *
 * 提供：
 *   - HttpRequest:   解析 HTTP 请求
 *   - HttpResponse:  构建 HTTP 响应
 *   - HttpRouter:    路由注册和匹配
 */
#pragma once

#include "coro_net.h"
#include <string>
#include <map>
#include <functional>
#include <sstream>
#include <algorithm>

// ==================== HttpRequest ====================

struct HttpRequest {
    std::string method;
    std::string path;
    std::string version;
    std::map<std::string, std::string> headers;
    std::string body;
    std::map<std::string, std::string> params;  // query params

    bool parse(const std::string& raw) {
        size_t pos = raw.find("\r\n");
        if (pos == std::string::npos) return false;

        // 请求行: GET /path?a=1 HTTP/1.1
        std::string request_line = raw.substr(0, pos);
        size_t sp1 = request_line.find(' ');
        size_t sp2 = request_line.rfind(' ');
        if (sp1 == std::string::npos || sp1 == sp2) return false;

        method = request_line.substr(0, sp1);
        std::string full_path = request_line.substr(sp1 + 1, sp2 - sp1 - 1);
        version = request_line.substr(sp2 + 1);

        // 分离 path 和 query string
        size_t qpos = full_path.find('?');
        if (qpos != std::string::npos) {
            path = full_path.substr(0, qpos);
            parse_query(full_path.substr(qpos + 1));
        } else {
            path = full_path;
        }

        // 解析 headers
        size_t hdr_start = pos + 2;
        while (true) {
            size_t line_end = raw.find("\r\n", hdr_start);
            if (line_end == std::string::npos || line_end == hdr_start) break;
            std::string line = raw.substr(hdr_start, line_end - hdr_start);
            size_t colon = line.find(':');
            if (colon != std::string::npos) {
                std::string key = line.substr(0, colon);
                std::string val = line.substr(colon + 1);
                // trim leading spaces
                while (!val.empty() && val[0] == ' ') val.erase(0, 1);
                // lowercase key
                std::transform(key.begin(), key.end(), key.begin(), ::tolower);
                headers[key] = val;
            }
            hdr_start = line_end + 2;
        }

        // body
        size_t body_start = raw.find("\r\n\r\n");
        if (body_start != std::string::npos) {
            body = raw.substr(body_start + 4);
        }

        return true;
    }

    // 从 body 中提取简易 JSON 字段（不引入第三方库）
    /*
        1. 找 "username"
        2. 找后面的 :
        3. 跳过空格
        4. 如果是字符串，就取两个双引号中间的内容
        5. 如果是数字/bool，就读到逗号或 }

        注意：这不是完整 JSON 解析器。它不处理复杂转义、嵌套对象、数组等情况。用于这个项目的登录注册简单请求够用。
    */
    std::string json_get(const std::string& key) const {
        std::string search = "\"" + key + "\"";
        size_t pos = body.find(search);
        if (pos == std::string::npos) return "";
        pos = body.find(':', pos + search.size());
        if (pos == std::string::npos) return "";
        pos++;
        while (pos < body.size() && (body[pos] == ' ' || body[pos] == '\t')) pos++;
        if (pos >= body.size()) return "";

        if (body[pos] == '"') {
            size_t end = body.find('"', pos + 1);
            if (end == std::string::npos) return "";
            return body.substr(pos + 1, end - pos - 1);
        }
        // number or bool
        size_t end = pos;
        while (end < body.size() && body[end] != ',' && body[end] != '}' &&
               body[end] != ' ' && body[end] != '\n')
            end++;
        return body.substr(pos, end - pos);
    }

    std::string header(const std::string& key) const {
        std::string lkey = key;
        std::transform(lkey.begin(), lkey.end(), lkey.begin(), ::tolower);
        auto it = headers.find(lkey);
        return it != headers.end() ? it->second : "";
    }

    std::string cookie(const std::string& name) const {
        std::string cookies = header("cookie");
        if (cookies.empty()) return "";
        std::string search = name + "=";
        size_t pos = cookies.find(search);
        if (pos == std::string::npos) return "";
        size_t start = pos + search.size();
        size_t end = cookies.find(';', start);
        if (end == std::string::npos) end = cookies.size();
        return cookies.substr(start, end - start);
    }

private:
    void parse_query(const std::string& qs) {
        size_t pos = 0;
        while (pos < qs.size()) {
            size_t amp = qs.find('&', pos);
            if (amp == std::string::npos) amp = qs.size();
            std::string pair = qs.substr(pos, amp - pos);
            size_t eq = pair.find('=');
            if (eq != std::string::npos) {
                params[url_decode(pair.substr(0, eq))] = url_decode(pair.substr(eq + 1));
            }
            pos = amp + 1;
        }
    }

    static std::string url_decode(const std::string& s) {
        std::string result;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '%' && i + 2 < s.size()) {
                int val = 0;
                std::sscanf(s.c_str() + i + 1, "%2x", &val);
                result += (char)val;
                i += 2;
            } else if (s[i] == '+') {
                result += ' ';
            } else {
                result += s[i];
            }
        }
        return result;
    }
};

// ==================== HttpResponse ====================

struct HttpResponse {
    int status = 200;
    std::string status_text = "OK";
    std::map<std::string, std::string> headers;
    std::string body;

    HttpResponse() {
        headers["Content-Type"] = "text/html; charset=utf-8";
    }

    static HttpResponse ok(const std::string& body, const std::string& content_type = "text/html; charset=utf-8") {
        HttpResponse r;
        r.body = body;
        r.headers["Content-Type"] = content_type;
        return r;
    }

    static HttpResponse json(const std::string& json_body) {
        return ok(json_body, "application/json");
    }

    static HttpResponse redirect(const std::string& url) {
        HttpResponse r;
        r.status = 302;
        r.status_text = "Found";
        r.headers["Location"] = url;
        return r;
    }

    static HttpResponse error(int code, const std::string& msg) {
        HttpResponse r;
        r.status = code;
        r.status_text = msg;
        r.body = "{\"error\":\"" + msg + "\"}";
        r.headers["Content-Type"] = "application/json";
        return r;
    }

    void set_cookie(const std::string& name, const std::string& value,
                    int max_age = 0, const std::string& path = "/") {
        std::string cookie = name + "=" + value + "; Path=" + path;
        if (max_age > 0)
            cookie += "; Max-Age=" + std::to_string(max_age);
        headers["Set-Cookie"] = cookie;
    }

    // 这是响应对象转成原始 HTTP 字符串的函数：
    std::string serialize() const {
        std::ostringstream oss;
        oss << "HTTP/1.1 " << status << " " << status_text << "\r\n";
        for (auto& [k, v] : headers) {
            oss << k << ": " << v << "\r\n";
        }
        oss << "Content-Length: " << body.size() << "\r\n";
        oss << "\r\n";
        oss << body;
        return oss.str();
    }
};

// ==================== JSON 工具 ====================

inline std::string json_escape(const std::string& s) {
    std::string r;
    r.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '"':  r += "\\\""; break;
            case '\\': r += "\\\\"; break;
            case '\n': r += "\\n"; break;
            case '\r': r += "\\r"; break;
            case '\t': r += "\\t"; break;
            default: r += c;
        }
    }
    return r;
}
