/**
 * async_mysql.h - 协程异步 MySQL 访问
 *
 * 线程池 + MySQL 连接池，协程以 co_await 方式异步等待 SQL 执行结果。
 * 连接不够时协程挂起（不阻塞 worker 线程），连接归还时自动恢复。
 *
 * 提供：
 *   - MySQLPool:        MySQL 连接池（内部使用 mutex，在线程池中阻塞等待）
 *   - ThreadPool:       通用线程池
 *   - MySQLResult:      查询结果
 *   - AsyncMySQLQuery:  co_await 异步 MySQL 查询
 *   - AsyncMySQLExec:   co_await 异步 MySQL 执行（INSERT/UPDATE/DELETE）
 */
#pragma once

#include "coro_net.h"
#include <mysql/mysql.h>
#include <functional>
#include <future>
#include <condition_variable>
#include <coroutine>

// ==================== 线程池 ====================

/*
    这是一个简单线程池，专门用来执行阻塞任务，比如 MySQL 查询。
    为什么需要它？
    因为这类代码会阻塞：mysql_real_query(...)
                      mysql_store_result(...)

    如果在 Worker 线程里直接执行，整个 Worker 就会卡住，无法处理其他客户端。
*/
class ThreadPool {
public:
    ThreadPool(int num_threads) : stop_(false) {
        for (int i = 0; i < num_threads; ++i) {
            workers_.emplace_back([this] {
                while (true) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(mutex_);
                        cv_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
                        if (stop_ && tasks_.empty()) return;
                        task = std::move(tasks_.front());
                        tasks_.pop();
                    }
                    task();
                }
            });
        }
    }

    ~ThreadPool() {
        { std::lock_guard<std::mutex> lock(mutex_); stop_ = true; }
        // 唤醒所有线程，然后等待所有线程结束
        cv_.notify_all();
        for (auto& t : workers_) t.join();
    }

    void submit(std::function<void()> task) {
        { std::lock_guard<std::mutex> lock(mutex_); tasks_.push(std::move(task)); }
        cv_.notify_one();
    }

private:
    std::vector<std::thread> workers_; // 线程池里的所有线程
    std::queue<std::function<void()>> tasks_; //任务队列
    std::mutex mutex_;
    std::condition_variable cv_; // 保护任务队列，并用于线程等待/唤醒。
    bool stop_;                  // 线程池是否准备停止。
};

// ==================== MySQL 连接池 ====================

class MySQLPool {
public:
    MySQLPool(const std::string& host, int port,
              const std::string& user, const std::string& password,
              const std::string& db, int pool_size)
        : host_(host), port_(port), user_(user), password_(password),
          db_(db), pool_size_(pool_size)
    {
        for (int i = 0; i < pool_size_; ++i) {
            MYSQL* conn = create_connection();
            if (conn) pool_.push(conn);
        }
        std::cout << "[MySQLPool] Created " << pool_.size() << " connections" << std::endl;
    }

    ~MySQLPool() {
        std::lock_guard<std::mutex> lock(mutex_);
        while (!pool_.empty()) {
            mysql_close(pool_.front());
            pool_.pop();
        }
    }

    MYSQL* acquire() {
        std::unique_lock<std::mutex> lock(mutex_);
        while (pool_.empty()) {
            cv_.wait(lock);
        }
        MYSQL* conn = pool_.front();
        pool_.pop();

        // 检查连接是否还活着。如果连接断了，就关闭旧连接，重新建一个。
        if (mysql_ping(conn) != 0) {
            mysql_close(conn);
            conn = create_connection();
        }
        return conn;
    }

    void release(MYSQL* conn) {
        if (!conn) return;
        std::lock_guard<std::mutex> lock(mutex_);
        pool_.push(conn);
        cv_.notify_one();
    }

private:
    MYSQL* create_connection() {
        MYSQL* conn = mysql_init(nullptr);
        if (!conn) return nullptr;

        unsigned int timeout = 5;
        mysql_options(conn, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
        mysql_options(conn, MYSQL_SET_CHARSET_NAME, "utf8mb4");

        if (!mysql_real_connect(conn, host_.c_str(), user_.c_str(),
                                password_.c_str(), db_.c_str(), port_,
                                nullptr, 0)) {
            std::cerr << "[MySQLPool] Connect failed: " << mysql_error(conn) << std::endl;
            mysql_close(conn);
            return nullptr;
        }
        return conn;
    }

    std::string host_;
    int port_;
    std::string user_, password_, db_;
    int pool_size_;
    std::queue<MYSQL *> pool_; // 空闲连接队列。
    std::mutex mutex_;
    std::condition_variable cv_; 
};

// ==================== MySQL 查询结果 ====================

struct MySQLResult {
    bool success = false; // SQL 是否成功。
    std::string error;    // 失败原因。
    std::vector<std::vector<std::string>> rows; // SELECT 查询结果
    uint64_t affected_rows = 0;                 // INSERT/UPDATE/DELETE 影响了多少行。
    uint64_t insert_id = 0;                     // 插入后生成的自增 ID。
};

// ==================== AsyncMySQLQuery ====================

class AsyncMySQLQuery {
public:
    AsyncMySQLQuery(Worker* worker, ThreadPool* tp, MySQLPool* pool,
                    const std::string& sql)
        : worker_(worker), tp_(tp), pool_(pool), sql_(sql) {}

    bool await_ready() { return false; }

    void await_suspend(std::coroutine_handle<> h) {
        handle_ = h;
        tp_->submit([this]() {
            MYSQL* conn = pool_->acquire();
            if (!conn) {
                result_.success = false;
                result_.error = "Failed to acquire MySQL connection";
                // 写入错误结果，然后恢复原协程
                worker_->schedule(handle_);
                return;
            }

            if (mysql_real_query(conn, sql_.c_str(), sql_.size()) != 0) {
                result_.success = false;
                result_.error = mysql_error(conn);
                pool_->release(conn);
                // 保存错误，释放连接，恢复协程。
                worker_->schedule(handle_);
                return;
            }

            MYSQL_RES* res = mysql_store_result(conn);
            if (res) {
                int num_fields = mysql_num_fields(res);
                MYSQL_ROW row;
                while ((row = mysql_fetch_row(res))) {
                    // 为什么需要长度？因为 MySQL 字段可能包含二进制数据或 \0，用长度构造字符串更准确。
                    unsigned long* lengths = mysql_fetch_lengths(res);
                    std::vector<std::string> r;
                    r.reserve(num_fields);
                    for (int i = 0; i < num_fields; ++i) {
                        r.emplace_back(row[i] ? std::string(row[i], lengths[i]) : "");
                    }
                    result_.rows.push_back(std::move(r));
                }
                //释放结果集，标记成功。
                mysql_free_result(res);
                result_.success = true;
            } 
            // 如果 mysql_store_result 返回空，不一定是错误。
            else {
                if (mysql_field_count(conn) == 0) {
                    result_.affected_rows = mysql_affected_rows(conn);
                    result_.insert_id = mysql_insert_id(conn);
                    result_.success = true;
                } else {
                    result_.success = false;
                    result_.error = mysql_error(conn);
                }
            }

            // 查询结束后，把连接还给连接池。
            pool_->release(conn);
            worker_->schedule(handle_);
        });
    }

    /*
        把原来挂起的业务协程调度回 Worker 线程。
        注意这里用的是 schedule，它会唤醒 Worker 的 eventfd，比 Redis 那边的 add_pending_resume 更主动。
    */
    MySQLResult await_resume() {
        return std::move(result_);
    }

private:
    Worker *worker_; // 原业务协程所在的 Worker。
    ThreadPool *tp_; // 执行阻塞 MySQL 查询的线程池。
    MySQLPool *pool_; // MySQL 连接池。
    std::string sql_; // 要执行的 SQL。
    MySQLResult result_; // 保存查询结果
    std::coroutine_handle<> handle_; // 等待这个 MySQL 查询的协程句柄
};

// 辅助：对 SQL 中的特殊字符进行转义（防 SQL 注入），降低SQL注入的风险，如果不转义，SQL 可能被拼坏。
inline std::string mysql_escape(const std::string& s) {
    std::string result;
    result.reserve(s.size() * 2);
    for (char c : s) {
        switch (c) {
            case '\'': result += "\\'"; break;
            case '"':  result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\0': result += "\\0"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\x1a': result += "\\Z"; break;
            default: result += c;
        }
    }
    return result;
}
