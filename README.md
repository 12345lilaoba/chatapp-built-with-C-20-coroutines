# ChatApp: 基于 C++20 协程的高并发 Web 聊天室

ChatApp 是一个完全从零手写的、基于 **C++20 无栈协程** 与 **epoll** 的高性能网络服务端项目。它不仅实现了一个功能完整的 Web 实时聊天室，更核心的是展示了如何利用现代 C++ 特性构建一套非阻塞的异步网络底层框架。

## ✨ 核心特性

* **自研协程网络底层**：基于 `epoll` 和 C++20 `co_await` 机制，实现了 `Worker` 事件循环调度器。用同步的编码风格实现了全异步的网络 I/O。
* **零依赖协议解析**：纯手写 HTTP 路由分发器和符合 RFC6455 规范的 WebSocket 协议层（包含握手、SHA-1/Base64、帧编码与掩码解析）。
* **全链路无阻塞设计**：
  * **Redis**：通过 `hiredis-async` 接入协程网络层，实现全异步的 Token 与历史消息读写。
  * **MySQL**：使用自建**线程池 + 连接池**，将阻塞式的 MySQL C API (注册/登录请求) 隔离在后台线程，并通过 `eventfd` 实现无阻塞跨线程唤醒协程。
* **自带压测工具**：项目内含多线程 HTTP 与 WebSocket 压测工具，方便进行并发容量与 QPS 测试。

## 🛠️ 环境依赖

* **编译器**：支持 C++20 标准的 GCC 或 Clang（建议 GCC 10+）
* **数据库**：MySQL 8.0+
* **缓存**：Redis
* **C/C++ 依赖库**：
  * `libmysqlclient-dev` (MySQL C API)
  * `libhiredis-dev` (Hiredis 异步库)

## 🚀 快速开始

### 1. 编译项目

在项目根目录下执行 `make` 即可编译出主服务 `chatserver` 以及压测工具。

```bash
make
```

### 2. 准备数据库环境

确保本机的 Redis 服务已启动。
确保 MySQL 服务已启动，并在运行前创建对应的数据库（如 `chatapp`）。

### 3. 启动聊天室服务

启动 `chatserver` 并传入必要的数据库凭据参数（第一次运行会自动在 MySQL 中建表）：

```bash
./chatserver --port 9090 --workers 4 --mysql-user <你的数据库账号> --mysql-pass <你的数据库密码> --mysql-db chatapp
```

* `--port`: 服务监听端口（默认 9090）
* `--workers`: 启动的底层 epoll 工作线程数
* `--mysql-*`: 数据库连接配置

### 4. 访问应用

服务启动后，打开浏览器访问以下地址即可进入前端页面，进行注册、登录和实时群聊体验：

```text
http://127.0.0.1:9090
```

## 🧪 压测与性能验证

项目内置了针对 HTTP 短连接/长连接以及 WebSocket 吞吐量的压测工具。

**测试 HTTP 注册接口 (QPS 测试):**
```bash
./benchmark -h 127.0.0.1 -p 9090 -t 8 -c 4 -n 1000 -m register -k
```

**测试 WebSocket 吞吐与延迟:**
```bash
./ws_bench
```

## 📖 深入阅读

如果你希望深入研究本项目的底层源码和架构设计，强烈建议阅读项目内附带的 [PROJECT_GUIDE.md](PROJECT_GUIDE.md)，其中包含了详细的组件职责图、协程运转流程以及核心链路解析。
