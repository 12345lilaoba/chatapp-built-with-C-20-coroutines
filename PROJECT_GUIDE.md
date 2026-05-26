# chatapp 项目彻底读懂指南

这份文档的目标不是替代源码注释，而是告诉你应该按什么顺序看、每一层要抓住什么问题、最后怎么验证自己真的看懂了。

## 1. 项目一句话概括

`chatapp` 是一个用 C++20 协程实现的高并发 Web 聊天室。

它同时包含：

- 一个 HTTP 服务端
- 一个 WebSocket 实时聊天服务
- 一个嵌入式 HTML/CSS/JS 前端页面
- Redis token 和聊天消息存储
- MySQL 用户注册/登录存储
- HTTP 和 WebSocket 压测工具
- 一套自写的 `epoll + coroutine` 网络调度层

最核心的学习价值是：看一个 C++20 协程网络服务器如何把 socket IO、Redis async、MySQL 阻塞查询和业务逻辑串成一个完整项目。

## 2. 先建立全局架构图

```text
Browser
  |
  | HTTP / WebSocket
  v
main.cpp
  |
  | accept 新连接
  | round-robin 分发
  v
Worker threads
  |
  | 每个 Worker:
  | - 一个 epoll fd
  | - 一个 eventfd 唤醒 fd
  | - 一组挂起/恢复的 coroutine_handle
  | - 一个 WorkerRedis 异步 Redis 连接
  v
handle_http_connection()
  |
  +-- HTTP 路由
  |   +-- GET  /
  |   +-- POST /api/register
  |   +-- POST /api/login
  |   +-- POST /api/send
  |   +-- GET  /api/messages
  |
  +-- WebSocket 升级
      +-- GET /ws?token=xxx
      +-- Redis 校验 token
      +-- 推送历史消息
      +-- 接收文本帧
      +-- Redis 保存消息
      +-- WSManager 广播

MySQL:
  users 表，保存 username/password_hash

Redis:
  token:<token> -> username
  chatroom:messages -> 最近聊天消息 list
```

## 3. 推荐阅读顺序

不要一上来就钻进协程细节。建议按下面顺序读。

### 第 1 步：看构建和运行入口

先看：

- `Makefile`
- `main.cpp`

重点问题：

- 编译出了哪些程序？
- `chatserver` 依赖哪些库？
- 命令行参数有哪些？
- 主线程做什么？
- Worker 线程什么时候创建？
- Redis 和 MySQL 什么时候初始化？
- 新连接如何分发给 Worker？

你应该先理解这个基本事实：

```text
main thread 只负责 accept。
真正处理连接的是 Worker 线程里的协程。
```

关键位置：

- `main.cpp`: 参数解析
- `main.cpp`: `init_mysql_tables`
- `main.cpp`: 创建 `g_workers`
- `main.cpp`: 创建 `g_worker_redis`
- `main.cpp`: `accept` 循环
- `main.cpp`: 调用 `handle_http_connection`

## 4. 文件职责地图

### 服务端核心

| 文件 | 作用 |
| --- | --- |
| `main.cpp` | 程序入口，初始化资源，启动 Worker，accept 新连接 |
| `coro_net.h` | 自写协程网络层，封装 Worker、AsyncRead、AsyncWrite、SwitchToWorker |
| `chat_app.h` | 业务核心，HTTP 路由、登录注册、WebSocket 消息循环 |
| `http_server.h` | 简易 HTTP 请求解析和响应构造 |
| `websocket.h` | WebSocket 握手、帧编码、连接管理、广播 |
| `async_redis.h` | hiredis-async 接入 Worker epoll，并包装成 co_await |
| `async_mysql.h` | MySQL 连接池、线程池、异步查询 awaiter |
| `chat_page.h` | 嵌入式前端 HTML/CSS/JS 页面 |

### 压测和测试

| 文件 | 作用 |
| --- | --- |
| `benchmark.cpp` | HTTP 注册/登录压测，支持短连接和 Keep-Alive |
| `ws_bench.cpp` | WebSocket 连接容量、吞吐和延迟压测 |
| `bench_layers.cpp` | 分层测试 MySQL、连接池、线程池开销 |
| `run_bench.sh` | 对比不同 workers/mysql-pool/thread-pool 参数 |
| `bench_pool.sh` | 对比连接池大小对注册 QPS 的影响 |
| `test_ws.py` | WebSocket 完整流程测试脚本 |

## 5. 运行时组件关系

### 全局资源

项目里有几个重要全局对象：

```cpp
ThreadPool* g_thread_pool;
MySQLPool*  g_mysql_pool;
std::vector<std::unique_ptr<Worker>> g_workers;
std::vector<std::unique_ptr<WorkerRedis>> g_worker_redis;
thread_local Worker* t_worker;
thread_local WorkerRedis* t_redis;
WSManager g_ws_manager;
```

理解它们的关系：

- `g_workers`: 每个 Worker 对应一个线程和一个 epoll loop
- `g_worker_redis`: 每个 Worker 一个 Redis async 连接
- `g_thread_pool`: 专门跑阻塞 MySQL 查询
- `g_mysql_pool`: MySQL 连接复用池
- `t_worker`: 当前线程正在使用的 Worker
- `t_redis`: 当前线程对应的 Redis 连接
- `g_ws_manager`: 全局在线 WebSocket 连接表

## 6. 协程网络层怎么读

核心文件：`coro_net.h`

建议按这个顺序看：

1. `FireAndForget`
2. `Worker`
3. `SwitchToWorker`
4. `AsyncRead`
5. `AsyncWrite`

### 6.1 FireAndForget 是什么

`FireAndForget` 是这个项目的协程返回类型。

它的特点：

- `initial_suspend` 是 `std::suspend_never`
- 调用协程函数后立即开始执行
- 不需要调用者 `co_await`
- `final_suspend` 也是 `std::suspend_never`
- 协程结束后自动释放协程帧

也就是说：

```cpp
handle_http_connection(client_fd, w, wr, g_thread_pool, g_mysql_pool);
```

这行代码看起来像普通函数调用，实际上会创建一个协程并开始执行。

### 6.2 Worker 是什么

`Worker` 是一个事件循环加协程调度器。

它维护：

- `epoll_fd_`: 等待 socket/Redis/eventfd 事件
- `wake_fd_`: 跨线程唤醒 Worker
- `handles_`: fd 到 coroutine handle 的映射
- `ready_events_`: 事件先到、协程还没挂起时的缓存
- `pending_resumes_`: 待恢复协程队列
- `fd_handlers_`: 自定义 fd handler，Redis 用这个挂进 epoll

你要重点理解这句话：

```text
fd 可读/可写时，Worker 从 handles_ 里找到对应协程，然后 resume。
```

### 6.3 AsyncRead / AsyncWrite 怎么工作

`AsyncRead` 的逻辑：

```text
await_ready:
  先直接 read 一次
  如果成功，协程不挂起
  如果 EAGAIN/EWOULDBLOCK，说明现在没数据，进入挂起

await_suspend:
  把当前 coroutine_handle 绑定到 fd
  修改 epoll 事件为 EPOLLIN

Worker::run:
  epoll_wait 等到 fd 可读
  找到 coroutine_handle
  resume 协程

await_resume:
  返回 read 结果
```

`AsyncWrite` 同理，只是监听 `EPOLLOUT`。

这是项目最重要的基础设施之一。看懂这里，后面的 HTTP/WebSocket 都会变简单。

## 7. HTTP 请求链路

核心函数：`handle_http_connection`，在 `chat_app.h`。

单个 HTTP 连接的大致流程：

```text
co_await SwitchToWorker
  |
  v
进入 Keep-Alive 循环
  |
  v
co_await AsyncRead 读请求头
  |
  v
解析 HttpRequest
  |
  v
如果有 body，继续读完整 body
  |
  v
判断是否 WebSocket Upgrade
  |
  +-- 是: 进入 WebSocket 分支
  |
  +-- 否: 普通 HTTP 路由
          |
          +-- 生成 HttpResponse
          +-- co_await AsyncWrite 写响应
          +-- 根据 keep-alive 决定是否继续
```

读这部分时，不要被代码长度吓到。它本质上只做三件事：

1. 收完整 HTTP 请求
2. 分发路由
3. 写回响应

## 8. HTTP 路由业务

### GET /

返回 `chat_page.h` 里的 `HTML_PAGE`。

这个项目没有单独的前端构建流程，页面直接以内嵌字符串形式编进二进制。

### POST /api/register

流程：

```text
解析 username/password
  |
  v
hash_password
  |
  v
INSERT IGNORE INTO users(...)
  |
  v
返回注册成功 / 用户名已存在
```

MySQL 表：

```sql
CREATE TABLE IF NOT EXISTS users (
  id BIGINT AUTO_INCREMENT PRIMARY KEY,
  username VARCHAR(64) NOT NULL UNIQUE,
  password_hash VARCHAR(64) NOT NULL,
  created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
```

### POST /api/login

流程：

```text
解析 username/password
  |
  v
hash_password
  |
  v
SELECT id FROM users WHERE username=... AND password_hash=...
  |
  v
生成 token
  |
  v
Redis SET token:<token> username
Redis EXPIRE token:<token> 3600
  |
  v
返回 token
```

### POST /api/send

HTTP 备用发送接口，主要聊天路径其实是 WebSocket。

流程：

```text
校验 token
  |
  v
写入 Redis list: chatroom:messages
```

### GET /api/messages

HTTP 备用拉历史消息接口。

流程：

```text
校验 token
  |
  v
LRANGE chatroom:messages -100 -1
  |
  v
组装 JSON 返回
```

## 9. WebSocket 链路

WebSocket 分支也在 `handle_http_connection` 里。

### 9.1 握手

浏览器请求：

```http
GET /ws?token=xxx HTTP/1.1
Upgrade: websocket
Connection: Upgrade
Sec-WebSocket-Key: ...
```

服务端做：

```text
取 query token
  |
  v
Redis GET token:<token>
  |
  +-- 无效: 返回 401
  |
  +-- 有效:
        计算 Sec-WebSocket-Accept
        返回 101 Switching Protocols
        g_ws_manager.add(...)
```

### 9.2 发送历史消息

握手成功后：

```text
LRANGE chatroom:messages -50 -1
  |
  v
逐条编码成 JSON
  |
  v
ws_encode_frame
  |
  v
AsyncWrite 发送给当前客户端
```

### 9.3 实时消息循环

核心逻辑：

```text
读 WebSocket frame header
  |
  v
读扩展长度
  |
  v
读 mask key
  |
  v
读 payload
  |
  v
解除 mask
  |
  +-- CLOSE: 断开
  +-- PING: 回复 PONG
  +-- TEXT:
        生成 "user|time|text"
        RPUSH chatroom:messages
        LTRIM chatroom:messages -200 -1
        g_ws_manager.broadcast(...)
```

### 9.4 WSManager

`WSManager` 在 `websocket.h`。

它维护一个：

```cpp
std::map<int, WSClient> clients_;
```

广播时遍历所有在线 fd，编码 WebSocket 文本帧，然后直接 `write`。

注意：这里广播没有使用协程异步写，而是直接非阻塞 `write`。这适合演示，但如果客户端很多、某些客户端发送缓冲区满，生产级实现需要更完整的发送队列和 EPOLLOUT 管理。

## 10. Redis 异步封装怎么读

核心文件：`async_redis.h`

这个文件的关键点是：hiredis-async 自己需要事件循环，而项目把它接到了 Worker 的 epoll 里。

阅读顺序：

1. `WorkerRedis` 构造函数
2. `ctx_->ev.addRead / delRead / addWrite / delWrite`
3. `worker_->register_fd_handler`
4. `AsyncRedisCommand`
5. `WorkerRedis::command_callback`

核心机制：

```text
hiredis 想监听读/写
  |
  v
调用 addRead/addWrite
  |
  v
WorkerRedis 修改 epoll 事件
  |
  v
epoll_wait 等到 Redis fd 可读/可写
  |
  v
调用 redisAsyncHandleRead/Write
  |
  v
hiredis 调用 command_callback
  |
  v
保存结果，恢复等待这个 Redis 命令的协程
```

你要看懂：

```cpp
co_await AsyncRedisCommand(redis, "GET %s", "token:" + token);
```

表面上像同步写法，底层实际是 Redis fd 事件驱动。

## 11. MySQL 异步封装怎么读

核心文件：`async_mysql.h`

MySQL C API 是阻塞的，所以项目没有把 MySQL fd 接进 epoll，而是用了线程池。

阅读顺序：

1. `ThreadPool`
2. `MySQLPool`
3. `MySQLResult`
4. `AsyncMySQLQuery`
5. `mysql_escape`

核心机制：

```text
业务协程 co_await AsyncMySQLQuery
  |
  v
await_suspend 把任务丢进 ThreadPool
  |
  v
线程池线程 acquire MySQL 连接
  |
  v
执行 mysql_real_query
  |
  v
保存 MySQLResult
  |
  v
释放连接
  |
  v
worker->schedule(handle)
  |
  v
Worker 恢复原业务协程
```

这一层最重要的思想是：

```text
阻塞 IO 不允许卡住 Worker。
所以用线程池把阻塞 MySQL 查询搬出去。
```

## 12. 前端怎么读

核心文件：`chat_page.h`

它是一个完整 HTML 页面，被 C++ 原始字符串字面量包住。

前端功能：

- 登录页
- 注册按钮
- 聊天主界面
- WebSocket 连接
- 自动重连
- 消息气泡渲染
- Enter 发送
- 移动端侧边栏

重点 JS 函数：

- `doRegister`
- `doLogin`
- `connectWebSocket`
- `doSend`
- `addBubble`
- `doLogout`

前后端对应关系：

| 前端函数 | 后端接口 |
| --- | --- |
| `doRegister` | `POST /api/register` |
| `doLogin` | `POST /api/login` |
| `connectWebSocket` | `GET /ws?token=...` |
| `doSend` | WebSocket text frame |

## 13. 压测工具怎么读

### benchmark.cpp

HTTP 注册/登录压测。

支持参数：

```bash
./benchmark -h 127.0.0.1 -p 9090 -t 8 -c 8 -n 1000 -m register -k
```

含义：

- `-h`: host
- `-p`: port
- `-t`: 线程数
- `-c`: 每线程连接数
- `-n`: 总请求数
- `-m`: `register` 或 `login`
- `-k`: 使用 Keep-Alive

它用多线程和 epoll 做压测客户端。

### ws_bench.cpp

WebSocket 压测。

测试维度：

- 连接容量
- 消息吞吐
- echo 延迟

它会先通过 HTTP 注册/登录拿 token，再建立 WebSocket。

### bench_layers.cpp

用于分析不同层的性能开销：

- Layer 1: 纯 MySQL C API INSERT
- Layer 2: 连接池 + 线程池 INSERT
- Layer 3: SELECT + INSERT，模拟注册完整路径

这个文件适合用来回答：

```text
性能瓶颈到底在 MySQL、线程池、连接池，还是 HTTP/协程网络层？
```

## 14. 推荐动手实验

### 实验 1：跑通服务

准备 Redis 和 MySQL，然后运行：

```bash
make
./chatserver --port 9090 --workers 4 --mysql-user chatuser --mysql-pass chatpass
```

浏览器打开：

```text
http://127.0.0.1:9090
```

注册两个用户，用两个浏览器窗口聊天。

### 实验 2：看 Redis 数据

登录后看 token：

```bash
redis-cli keys 'token:*'
redis-cli get token:<你的token>
```

发消息后看消息列表：

```bash
redis-cli lrange chatroom:messages 0 -1
```

### 实验 3：看 MySQL 数据

```sql
USE chatapp;
SELECT * FROM users;
```

确认注册用户进入 `users` 表。

### 实验 4：HTTP 压测

```bash
./benchmark -h 127.0.0.1 -p 9090 -t 8 -c 4 -n 1000 -m register -k
./benchmark -h 127.0.0.1 -p 9090 -t 8 -c 4 -n 1000 -m login -k
```

观察：

- QPS
- 平均延迟
- failed 数量
- Keep-Alive 和短连接差异

### 实验 5：调 Worker/MySQL/线程池参数

```bash
./run_bench.sh
```

观察不同组合：

- `--workers`
- `--mysql-pool`
- `--thread-pool`

对注册和登录 QPS 的影响。

## 15. 读源码时要追的 5 条主线

### 主线 1：一个 HTTP 注册请求怎么走

```text
浏览器 fetch /api/register
  -> main accept
  -> 分配 Worker
  -> handle_http_connection
  -> HttpRequest::parse
  -> route /api/register
  -> AsyncMySQLQuery
  -> ThreadPool
  -> MySQLPool
  -> mysql_real_query
  -> worker->schedule
  -> HttpResponse::serialize
  -> AsyncWrite
```

### 主线 2：一次登录怎么走

```text
POST /api/login
  -> MySQL 校验 username/password_hash
  -> generate_token
  -> Redis SET token
  -> Redis EXPIRE token
  -> 返回 token 给前端
```

### 主线 3：WebSocket 连接怎么建立

```text
new WebSocket('/ws?token=...')
  -> HTTP Upgrade
  -> Redis GET token
  -> 计算 Sec-WebSocket-Accept
  -> 返回 101
  -> g_ws_manager.add
  -> 推送历史消息
```

### 主线 4：一条聊天消息怎么广播

```text
浏览器 ws.send(text)
  -> 服务端读 WebSocket frame
  -> 解除 mask
  -> Redis RPUSH
  -> Redis LTRIM
  -> 拼 JSON
  -> g_ws_manager.broadcast
  -> 所有在线客户端收到消息
```

### 主线 5：协程什么时候挂起，什么时候恢复

```text
AsyncRead 没数据
  -> 挂起协程
  -> fd 绑定 coroutine_handle
  -> epoll_wait 等 EPOLLIN
  -> Worker resume

AsyncRedisCommand
  -> 提交 hiredis async 命令
  -> Redis fd 事件到来
  -> callback 保存结果
  -> Worker resume

AsyncMySQLQuery
  -> 提交线程池任务
  -> MySQL 查询完成
  -> worker->schedule
  -> Worker resume
```

如果这 5 条主线都能从代码里手动画出来，你就真的掌握这个项目了。

## 16. 需要特别注意的设计取舍

### 16.1 密码哈希只是演示级

`hash_password` 使用简化 FNV-1a 哈希，没有盐，也不是密码学安全方案。

生产环境应使用 bcrypt、argon2、scrypt 等。

### 16.2 JSON 解析是简易字符串查找

`HttpRequest::json_get` 没有完整 JSON parser，只适合简单 demo。

复杂输入、转义字符、嵌套结构都不可靠。

### 16.3 SQL 拼接有风险

项目做了 `mysql_escape`，但更好的生产做法是 prepared statement。

### 16.4 WebSocket 广播是同步 write

`WSManager::broadcast` 直接遍历 fd 并 `write`，没有 per-client 发送队列。

如果某个客户端慢，或者 socket send buffer 满，消息可能发送不完整或丢失。

### 16.5 HTTP parser 是教学版

能处理基本请求、Content-Length、Keep-Alive，但不是完整 HTTP 协议实现。

### 16.6 test_ws.py 里有一个疑点

`test_ws.py` 里计算 WebSocket Accept 的 GUID 看起来不是标准 GUID。

服务端 `websocket.h` 使用的是标准 GUID：

```text
258EAFA5-E914-47DA-95CA-C5AB0DC85B11
```

如果 `test_ws.py` 断言 Accept mismatch，优先检查这里。

## 17. 可以怎么画自己的笔记

建议你自己补三张图：

### 图 1：线程模型图

画出：

- main thread
- Worker 0..N
- ThreadPool 0..N
- Redis connections
- MySQL connections

### 图 2：协程挂起恢复图

画出：

- `co_await AsyncRead`
- `Worker::set_client_handle`
- `epoll_wait`
- `resume`

### 图 3：聊天消息时序图

画出：

- Alice 浏览器
- Server Worker
- Redis
- Bob 浏览器

从 Alice `ws.send` 到 Bob 收到消息。

## 18. 最终自测问题

如果你能回答下面问题，说明你基本读懂了：

1. 为什么每个 Worker 要有自己的 Redis 连接？
2. 为什么 MySQL 不直接在 Worker 线程里查？
3. `AsyncRead` 在什么时候不会挂起？
4. `eventfd` 在这个项目里解决什么问题？
5. HTTP Keep-Alive 的 `leftover` 是干什么的？
6. WebSocket 客户端发来的 frame 为什么要 mask？
7. 聊天历史为什么存在 Redis，而不是 MySQL？
8. `g_ws_manager.broadcast` 有什么潜在问题？
9. 注册接口为什么用 `INSERT IGNORE`？
10. 如果要支持多个聊天室，你会改哪些数据结构和接口？

## 19. 一条最短掌握路线

如果时间有限，按这个路线：

```text
Makefile
  -> main.cpp
  -> coro_net.h
  -> chat_app.h 的 handle_http_connection
  -> async_redis.h
  -> async_mysql.h
  -> websocket.h
  -> chat_page.h
  -> benchmark.cpp / ws_bench.cpp
```

每读完一个文件，都回到这句话确认理解：

```text
这个文件在请求链路的哪一段？
它是在处理 IO、调度、协议、存储，还是业务？
```

能回答这个问题，项目的结构就不会乱。

