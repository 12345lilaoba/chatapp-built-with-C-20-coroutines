/**
 * async_redis.h - 协程异步 Redis 访问
 *
 * 基于 hiredis-async，每个 Worker 一个异步连接，所有协程共享。
 * 提供：
 *   - RedisAwaiter:       Redis 命令结果
 *   - AsyncRedisCommand:  co_await 异步 Redis 命令
 *   - WorkerRedis:        Worker 级 Redis 连接管理（含 hiredis-async 适配器）
 */
#pragma once

#include "coro_net.h"
#include <hiredis/hiredis.h>
#include <hiredis/async.h>

// ==================== RedisAwaiter ====================

struct RedisAwaiter {
    std::coroutine_handle<> handle;    //等待 Redis 结果的协程句柄
    int reply_type = 0;                // Redis 返回类型
    std::string reply_str;             // 字符串结果
    long long reply_integer = 0;       // 整数结果
    bool completed = false;            // 是否完成
};

// ==================== WorkerRedis ====================

class WorkerRedis {
public:
    WorkerRedis(Worker* worker, const std::string& host, int port,
                const std::string& password = "")
        : worker_(worker)
    {
        // 这行创建 Redis 异步连接。注意，不是启动 Redis 服务，而是客户端连接 Redis 服务。
        ctx_ = redisAsyncConnect(host.c_str(), port);
        if (!ctx_ || ctx_->err) {
            std::cerr << "Worker " << worker->id() << " Redis connect failed: "
                      << (ctx_ ? ctx_->errstr : "null") << std::endl;
            ctx_ = nullptr;
            return;
        }

        /*
            ctx_ 是 hiredis 的上下文。hiredis 的回调函数是 C 风格静态函数，它本身不知道当前属于哪个 WorkerRedis 对象。
            所以这里把 this 存进去。后面 callback 里可以这样拿回来
        */

        ctx_->data = this;

        ctx_->ev.data = this;
        ctx_->ev.addRead  = [](void* p) { auto* s = (WorkerRedis*)p; s->reading_ = true;  s->update_events(); };
        ctx_->ev.delRead  = [](void* p) { auto* s = (WorkerRedis*)p; s->reading_ = false; s->update_events(); };
        ctx_->ev.addWrite = [](void* p) { auto* s = (WorkerRedis*)p; s->writing_ = true;  s->update_events(); };
        ctx_->ev.delWrite = [](void* p) { auto* s = (WorkerRedis*)p; s->writing_ = false; s->update_events(); };
        ctx_->ev.cleanup  = [](void* p) { auto* s = (WorkerRedis*)p; s->cleanup(); };

        fd_ = ctx_->c.fd;

        epoll_event ev{};
        ev.events = 0;
        ev.data.fd = fd_;
        epoll_ctl(worker_->epoll_fd(), EPOLL_CTL_ADD, fd_, &ev);
        in_epoll_ = true;

        // Redis fd 可读：Redis 服务器已经返回数据了，这个 socket 里有数据可以读。
        // Redis fd 可写：如果 Redis fd 当前可写，说明内核发送缓冲区有空间，可以把这段命令发出去。
        worker_->register_fd_handler(fd_, [this](uint32_t events) {
            if (events & EPOLLIN)  redisAsyncHandleRead(ctx_);
            if (events & EPOLLOUT) redisAsyncHandleWrite(ctx_);
        });

        if (!password.empty()) {
            int rc = redisAsyncCommand(ctx_, WorkerRedis::auth_callback,
                                       nullptr, "AUTH %s", password.c_str());
            if (rc != REDIS_OK) {
                std::cerr << "Worker " << worker->id()
                          << " Redis auth command failed" << std::endl;
            }
        }
    }

    ~WorkerRedis() {
        if (ctx_) {
            worker_->unregister_fd_handler(fd_);
            redisAsyncDisconnect(ctx_);
        }
    }

    redisAsyncContext* ctx() { return ctx_; }
    Worker* worker() { return worker_; }

    static void auth_callback(redisAsyncContext* c, void* r, void*) {
        auto* wr = (WorkerRedis*)c->data;
        auto* reply = (redisReply*)r;
        if (!reply || reply->type == REDIS_REPLY_ERROR) {
            std::cerr << "Worker " << wr->worker_->id() << " Redis auth failed: "
                      << (reply && reply->str ? reply->str : "no reply")
                      << std::endl;
        }
    }


    static void command_callback(redisAsyncContext* c, void* r, void* privdata) {
        auto* awaiter = (RedisAwaiter*)privdata;
        auto* reply = (redisReply*)r;

        if (reply) {
            awaiter->reply_type = reply->type;
            if (reply->type == REDIS_REPLY_STRING && reply->str)
                awaiter->reply_str = std::string(reply->str, reply->len);
            else if (reply->type == REDIS_REPLY_INTEGER)
                awaiter->reply_integer = reply->integer;
            else if (reply->type == REDIS_REPLY_ARRAY) {
                // 将数组元素用 \n 拼接（简单处理 LRANGE 等命令的返回）
                std::string result;
                for (size_t i = 0; i < reply->elements; ++i) {
                    if (reply->element[i] && reply->element[i]->type == REDIS_REPLY_STRING) {
                        if (!result.empty()) result += "\n";
                        result += std::string(reply->element[i]->str, reply->element[i]->len);
                    }
                }
                awaiter->reply_str = std::move(result);
                awaiter->reply_type = REDIS_REPLY_ARRAY;
            }
        }
        awaiter->completed = true;

        auto* wr = (WorkerRedis*)c->data;
        // Redis 回来了，把之前 co_await 挂起的协程放回 Worker 的待恢复队列。
        wr->worker_->add_pending_resume(awaiter->handle);
    }

private:
    void update_events() {
        if (!in_epoll_ || fd_ < 0) return;
        uint32_t events = 0;
        if (reading_) events |= EPOLLIN;
        if (writing_) events |= EPOLLOUT;
        epoll_event ev{};
        ev.events = events;
        ev.data.fd = fd_;
        epoll_ctl(worker_->epoll_fd(), EPOLL_CTL_MOD, fd_, &ev);
    }

    void cleanup() {
        if (in_epoll_ && fd_ >= 0) {
            epoll_ctl(worker_->epoll_fd(), EPOLL_CTL_DEL, fd_, nullptr);
            in_epoll_ = false;
        }
    }

    Worker* worker_;
    redisAsyncContext* ctx_ = nullptr;
    int fd_ = -1;
    bool reading_ = false;      // 当前是否需要监听读事件。
    bool writing_ = false;      // 当前是否需要监听写事件。
    bool in_epoll_ = false;     //当前 fd 是否已经加入 epoll。

};

// ==================== AsyncRedisCommand ====================

class AsyncRedisCommand {
public:
    // 无参命令
    AsyncRedisCommand(WorkerRedis* wr, const char* cmd)
        : wr_(wr), cmd_(cmd) {
        awaiter_ = std::make_shared<RedisAwaiter>();
    }

    // 1 参命令
    AsyncRedisCommand(WorkerRedis* wr, const char* fmt, const std::string& a1)
        : wr_(wr), fmt_(fmt), a1_(a1) {
        awaiter_ = std::make_shared<RedisAwaiter>();
    }

    // 2 参命令
    AsyncRedisCommand(WorkerRedis* wr, const char* fmt,
                      const std::string& a1, const std::string& a2)
        : wr_(wr), fmt_(fmt), a1_(a1), a2_(a2) {
        awaiter_ = std::make_shared<RedisAwaiter>();
    }

    // 3 参命令
    AsyncRedisCommand(WorkerRedis* wr, const char* fmt,
                      const std::string& a1, const std::string& a2,
                      const std::string& a3)
        : wr_(wr), fmt_(fmt), a1_(a1), a2_(a2), a3_(a3) {
        awaiter_ = std::make_shared<RedisAwaiter>();
    }

    //这个 awaiter 不会立即完成。
    //每次 co_await Redis 命令，都进入 await_suspend 挂起协程。
    bool await_ready() { return false; }

    void await_suspend(std::coroutine_handle<> h) {
        awaiter_->handle = h;
        int rc = REDIS_ERR;

        /*
            下面是做：
                提交 Redis 命令
                保存协程 handle
                等待 Redis 回调恢复协程
        */
        /*
            await_suspend 中 redisAsyncCommand 被调用了；
            但它只是“提交异步命令”，不是“执行完 Redis 命令并拿到结果”。
        */
        if (!fmt_) {
            rc = redisAsyncCommand(wr_->ctx(), WorkerRedis::command_callback,
                                   awaiter_.get(), cmd_);
        } else if (a2_.empty() && a3_.empty()) {
            rc = redisAsyncCommand(wr_->ctx(), WorkerRedis::command_callback,
                                   awaiter_.get(), fmt_, a1_.c_str());
        } else if (a3_.empty()) {
            rc = redisAsyncCommand(wr_->ctx(), WorkerRedis::command_callback,
                                   awaiter_.get(), fmt_, a1_.c_str(), a2_.c_str());
        } else {
            rc = redisAsyncCommand(wr_->ctx(), WorkerRedis::command_callback,
                                   awaiter_.get(), fmt_, a1_.c_str(), a2_.c_str(), a3_.c_str());
        }

        // 如果提交命令失败，就手动构造一个错误结果，并把协程放回待恢复队列，避免协程永远挂住。
        /*
            但这里有一个细节：如果这个失败发生在 process_pending_resumes() 正在恢复某个协程的过程中，新加入的 handle 会进入全局 pending_resumes_，而不是当前那份本地 to_resume 队列。它可能要等下一轮 process_pending_resumes() 才会被处理。
            所以通过schedule调用会唤醒Worker，可以保证即使 Worker 马上回到 epoll_wait，也会被 wake_fd_ 唤醒。
        */
        if (rc != REDIS_OK) {
            awaiter_->reply_type = REDIS_REPLY_ERROR;
            awaiter_->reply_str = "redisAsyncCommand failed";
            awaiter_->completed = true;
            wr_->worker()->schedule(h);
        }
    }

    std::shared_ptr<RedisAwaiter> await_resume() {
        return awaiter_;
    }

private:
    WorkerRedis* wr_;
    const char* cmd_ = nullptr;
    const char* fmt_ = nullptr;
    std::string a1_, a2_, a3_;
    std::shared_ptr<RedisAwaiter> awaiter_;
};
