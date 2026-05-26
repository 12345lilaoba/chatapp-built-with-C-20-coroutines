/**
 * coro_net.h - 协程网络层
 *
 * 从 server_coro_async_redis.cpp 抽取，提供：
 *   - FireAndForget: 协程返回类型（启动即忘）
 *   - Worker:        epoll 事件循环 + 协程调度
 *   - AsyncRead:     异步 socket 读
 *   - AsyncWrite:    异步 socket 写
 *   - SwitchToWorker: 跨线程调度 awaiter
 */
#pragma once

#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <coroutine>
#include <cstring>
#include <string>
#include <iostream>
#include <vector>
#include <map>
#include <queue>
#include <mutex>
#include <thread>
#include <memory>
#include <atomic>
#include <functional>
#include <chrono>

// ==================== 工具函数 ====================

inline void set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

inline int create_server(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        perror("socket");
        return -1;
    }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(fd, (sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("bind");
        close(fd);
        return -1;
    }
    if (listen(fd, 1024) < 0)
    {
        perror("listen");
        close(fd);
        return -1;
    }

    return fd;
}

// ==================== 协程返回类型 ====================

class FireAndForget
{
public:
    struct promise_type
    {
        FireAndForget get_return_object() { return {}; }
        std::suspend_never initial_suspend() { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() { std::terminate(); }
    };
};

// ==================== Worker 事件循环 ====================

class Worker
{
public:
    Worker(int id) : id_(id)
    {
        epoll_fd_ = epoll_create1(0);
        wake_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (wake_fd_ >= 0)
        {
            epoll_event ev{};
            ev.events = EPOLLIN;
            ev.data.fd = wake_fd_;
            epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, wake_fd_, &ev);
        }
    }

    ~Worker()
    {
        if (wake_fd_ >= 0)
            close(wake_fd_);
        if (epoll_fd_ >= 0)
            close(epoll_fd_);
    }

    int id() const { return id_; }
    int epoll_fd() const { return epoll_fd_; }

    // ---------- fd 管理 ----------

    // 把一个新的客户端 fd 加入当前 Worker 的 epoll。
    // 注意：这里的 fd 只是开始被 epoll 监听，还不代表已经有协程在 handles_ 中等待它。
    void add_client_fd(int fd, uint32_t events)
    {
        epoll_event ev{};
        ev.events = events;
        ev.data.fd = fd;
        epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev);
    }

    void mod_client_fd(int fd, uint32_t events)
    {
        epoll_event ev{};
        ev.events = events;
        ev.data.fd = fd;
        epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev);
    }

    void del_client_fd(int fd)
    {
        epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
        std::lock_guard<std::mutex> lock(handles_mutex_);
        handles_.erase(fd);
        ready_events_.erase(fd);
    }

    void clear_ready_events(int fd)
    {
        std::lock_guard<std::mutex> lock(handles_mutex_);
        ready_events_.erase(fd);
    }

    // ---------- 协程句柄管理 ----------

    void set_client_handle(int fd, std::coroutine_handle<> h)
    {
        std::coroutine_handle<> to_resume;
        {
            std::lock_guard<std::mutex> lock(handles_mutex_);
            // handles_ 不是 fd 的“归属表”，只表示：当前这个协程正挂起等待该 fd。
            // 协程正在运行或还没执行到 co_await AsyncRead/AsyncWrite 时，这里可能没有有效 handle。
            handles_[fd] = h;
            // 如果 epoll 事件早于协程注册到 handles_，run() 会先把事件放进 ready_events_。
            // 现在协程来登记等待这个 fd，就要检查是否已有“提前到达”的事件。
            auto it = ready_events_.find(fd);
            if (it != ready_events_.end())
            {
                if (it->second & (EPOLLIN | EPOLLOUT))
                {
                    it->second = 0;
                    ready_events_.erase(it);
                    // 说明这个 fd 之前已经就绪过了，不必再等下一次 epoll 通知。
                    // 为了避免在持有 handles_mutex_ 时直接 resume，这里先记录到 to_resume。
                    to_resume = h;
                    handles_[fd] = nullptr;
                }
            }
        }
        if (to_resume)
        {
            // 事件已经提前到达，将协程放入待恢复队列，稍后由 Worker 统一恢复。
            add_pending_resume(to_resume);
        }
    }

    // ---------- 跨线程调度 ----------

    void schedule(std::coroutine_handle<> h)
    {
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            pending_resumes_.push(h);
        }
        wakeup();
    }

    void add_pending_resume(std::coroutine_handle<> h)
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_resumes_.push(h);
    }

    void wakeup()
    {
        if (wake_fd_ < 0)
            return;
        uint64_t one = 1;
        ssize_t ret = ::write(wake_fd_, &one, sizeof(one));
        (void)ret;
    }

    // ---------- 自定义 fd 事件处理（给 Redis async 用）----------

    using FdHandler = std::function<void(uint32_t events)>;

    void register_fd_handler(int fd, FdHandler handler)
    {
        std::lock_guard<std::mutex> lock(fd_handlers_mutex_);
        fd_handlers_[fd] = std::move(handler);
    }

    void unregister_fd_handler(int fd)
    {
        std::lock_guard<std::mutex> lock(fd_handlers_mutex_);
        fd_handlers_.erase(fd);
    }

    // ---------- 事件循环 ----------

    void run()
    {
        epoll_event events[256];

        while (running_)
        {
            int n = epoll_wait(epoll_fd_, events, 256, -1);

            for (int i = 0; i < n; ++i)
            {
                int fd = events[i].data.fd;
                uint32_t ev = events[i].events;

                if (fd == wake_fd_)
                {
                    uint64_t v;
                    while (::read(wake_fd_, &v, sizeof(v)) > 0)
                    {
                    }
                    process_pending_resumes();
                    continue;
                }

                // 检查是否有自定义 fd handler（Redis fd 等）
                {
                    std::lock_guard<std::mutex> lock(fd_handlers_mutex_);
                    auto it = fd_handlers_.find(fd);
                    if (it != fd_handlers_.end())
                    {
                        it->second(ev);
                        continue;
                    }
                }

                // 客户端 fd 事件 -> 恢复对应协程
                std::coroutine_handle<> h;
                {
                    std::lock_guard<std::mutex> lock(handles_mutex_);
                    // epoll 已经报告这个客户端 fd 有事件。
                    // 只有当协程正挂起等待该 fd 时，handles_[fd] 才会有可恢复的 handle。
                    auto it = handles_.find(fd);
                    if (it != handles_.end() && it->second)
                    {
                        h = it->second;
                        it->second = nullptr;
                    }
                    else
                    {
                        // 没有协程正在等这个 fd，不能直接丢掉这个 epoll 事件。
                        // 先暂存到 ready_events_，等协程之后执行 co_await 并注册 handle 时再补一次恢复。
                        ready_events_[fd] |= ev;
                    }
                }
                if (h && !h.done())
                {
                    h.resume();
                }
            }

            process_pending_resumes();
        }
    }

    void stop() { running_ = false; }

private:
    void process_pending_resumes()
    {
        std::queue<std::coroutine_handle<>> to_resume;
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            std::swap(to_resume, pending_resumes_);
        }
        while (!to_resume.empty())
        {
            auto h = to_resume.front();
            to_resume.pop();
            if (h && !h.done())
            {
                h.resume();
            }
        }
    }

    /*
        handles_
        条件还没满足
        协程正在等待某个 fd 事件

        pending_resumes_
        条件已经满足
        协程等待被 Worker resume

        pending_resumes_条件
        （
            1. 跨线程 schedule(h)
            目标 Worker 应该恢复这个协程了

            2. Redis 回调拿到结果
            等 Redis 的协程可以继续了

            3. ready_events_ 发现 fd 事件已经提前到达
            等 fd 的协程不需要再等 epoll 了

            4. eventfd 唤醒后要处理的已投递协程
        ）

        ready_events_
        条件已经满足得太早
        但当时协程还没登记等待，所以先记一笔
    */

    int id_;
    int epoll_fd_;     // 这个 Worker 自己的 epoll 实例
    int wake_fd_ = -1; // eventfd，用来跨线程唤醒 epoll_wait

    std::map<int, std::coroutine_handle<>> handles_; // fd -> 当前正挂起等待该 fd 的协程句柄，不表示 fd 永久归属
    std::map<int, uint32_t> ready_events_;           // epoll 事件早于协程注册时，临时保存该 fd 已经就绪过
    std::mutex handles_mutex_;

    std::queue<std::coroutine_handle<>> pending_resumes_; // 等待恢复执行的协程队列
    std::mutex pending_mutex_;

    std::map<int, FdHandler> fd_handlers_; // 自定义 fd 处理器，主要给 Redis async 用
    std::mutex fd_handlers_mutex_;

    bool running_ = true;
};

// 全局 worker 列表和 thread_local 当前 worker行 await_suspend()
inline std::vector<std::unique_ptr<Worker>> g_workers;
inline thread_local Worker *t_worker = nullptr;

// ==================== SwitchToWorker ====================

struct SwitchToWorker
{
    Worker *worker;
    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> h) const noexcept
    {
        worker->schedule(h);
    }
    void await_resume() const noexcept {}
};

// ==================== AsyncRead / AsyncWrite ====================

class AsyncRead
{
public:
    AsyncRead(int fd, char *buf, size_t len) : fd_(fd), buf_(buf), len_(len) {}

    bool await_ready()
    {
        result_ = ::read(fd_, buf_, len_);
        if (result_ >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
        {
            // 操作立即完成，清除可能残留的 stale ready_events
            if (t_worker)
                t_worker->clear_ready_events(fd_);
            return true;
        }
        return false;
    }

    void await_suspend(std::coroutine_handle<> h)
    {
        t_worker->set_client_handle(fd_, h);
        t_worker->mod_client_fd(fd_, EPOLLIN | EPOLLET);
    }

    ssize_t await_resume()
    {
        if (result_ < 0)
            result_ = ::read(fd_, buf_, len_);
        return result_;
    }

private:
    int fd_;
    char *buf_;
    size_t len_;
    ssize_t result_ = -1;
};

class AsyncWrite
{
public:
    AsyncWrite(int fd, const char *data, size_t len)
        : fd_(fd), data_(data), len_(len) {}

    bool await_ready()
    {
        result_ = ::write(fd_, data_, len_);
        if (result_ >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
        {
            if (t_worker)
                t_worker->clear_ready_events(fd_);
            return true;
        }
        return false;
    }

    void await_suspend(std::coroutine_handle<> h)
    {
        t_worker->set_client_handle(fd_, h);
        t_worker->mod_client_fd(fd_, EPOLLOUT | EPOLLET);
    }

    ssize_t await_resume()
    {
        if (result_ < 0)
            result_ = ::write(fd_, data_, len_);
        return result_;
    }

private:
    int fd_;
    const char *data_;
    size_t len_;
    ssize_t result_ = -1;
};

// 辅助：完整写入（处理 partial write）
inline FireAndForget async_write_all(int fd, std::string data,
                                     std::function<void(bool)> on_done = nullptr)
{
    // 注意：这不是 awaitable 的，只是一个便利协程
    // 业务代码应该自己循环写
    size_t off = 0;
    while (off < data.size())
    {
        ssize_t wn = co_await AsyncWrite(fd, data.data() + off, data.size() - off);
        if (wn <= 0)
        {
            if (wn < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
                continue;
            if (on_done)
                on_done(false);
            co_return;
        }
        off += (size_t)wn;
    }
    if (on_done)
        on_done(true);
}
