/**
 * bench_layers.cpp - 分层性能测试
 *
 * 逐层测试各组件开销：
 *   Layer 1: 纯 MySQL C API INSERT（多线程直连）
 *   Layer 2: MySQL 连接池 + 线程池 INSERT（模拟协程应用的 MySQL 路径）
 *   Layer 3: 注册完整路径 SELECT + INSERT（通过线程池）
 */

#include <mysql/mysql.h>
#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <cstring>

std::atomic<int64_t> g_done{0};

// ==================== Layer 1: 纯 MySQL C API INSERT ====================

void bench_raw_mysql(const char* host, int port, const char* user,
                     const char* pass, const char* db,
                     int thread_id, int count) {
    MYSQL* conn = mysql_init(nullptr);
    unsigned int timeout = 5;
    mysql_options(conn, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
    mysql_options(conn, MYSQL_SET_CHARSET_NAME, "utf8mb4");

    if (!mysql_real_connect(conn, host, user, pass, db, port, nullptr, 0)) {
        std::cerr << "Connect failed: " << mysql_error(conn) << std::endl;
        return;
    }

    for (int i = 0; i < count; ++i) {
        char sql[256];
        snprintf(sql, sizeof(sql),
                 "INSERT INTO users(username, password_hash) VALUES('raw_%d_%d', 'hash_%d')",
                 thread_id, i, i);
        if (mysql_real_query(conn, sql, strlen(sql)) != 0) {
            // 忽略重复键等错误
        }
        g_done++;
    }
    mysql_close(conn);
}

// ==================== Layer 2: 连接池 + 线程池 INSERT ====================

class SimplePool {
public:
    SimplePool(const char* host, int port, const char* user,
               const char* pass, const char* db, int size) {
        for (int i = 0; i < size; ++i) {
            MYSQL* c = mysql_init(nullptr);
            unsigned int timeout = 5;
            mysql_options(c, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
            mysql_options(c, MYSQL_SET_CHARSET_NAME, "utf8mb4");
            if (mysql_real_connect(c, host, user, pass, db, port, nullptr, 0))
                pool_.push(c);
            else { mysql_close(c); }
        }
    }
    ~SimplePool() { while (!pool_.empty()) { mysql_close(pool_.front()); pool_.pop(); } }

    MYSQL* acquire() {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait(lk, [&]{ return !pool_.empty(); });
        auto c = pool_.front(); pool_.pop(); return c;
    }
    void release(MYSQL* c) {
        std::lock_guard<std::mutex> lk(mu_);
        pool_.push(c); cv_.notify_one();
    }
private:
    std::queue<MYSQL*> pool_;
    std::mutex mu_;
    std::condition_variable cv_;
};

class SimpleTP {
public:
    SimpleTP(int n) : stop_(false) {
        for (int i = 0; i < n; ++i)
            w_.emplace_back([this]{
                while (true) {
                    std::function<void()> t;
                    { std::unique_lock<std::mutex> lk(mu_);
                      cv_.wait(lk, [&]{ return stop_ || !q_.empty(); });
                      if (stop_ && q_.empty()) return;
                      t = std::move(q_.front()); q_.pop(); }
                    t();
                }
            });
    }
    ~SimpleTP() { { std::lock_guard<std::mutex> lk(mu_); stop_ = true; }
                  cv_.notify_all(); for (auto& t : w_) t.join(); }
    void submit(std::function<void()> f) {
        { std::lock_guard<std::mutex> lk(mu_); q_.push(std::move(f)); }
        cv_.notify_one();
    }
private:
    std::vector<std::thread> w_;
    std::queue<std::function<void()>> q_;
    std::mutex mu_; std::condition_variable cv_; bool stop_;
};

std::atomic<int64_t> g_pool_done{0};

void bench_pool_insert(SimpleTP& tp, SimplePool& pool, int total) {
    std::atomic<int> pending{total};
    std::mutex done_mu;
    std::condition_variable done_cv;

    for (int i = 0; i < total; ++i) {
        tp.submit([&pool, &pending, &done_cv, i]() {
            MYSQL* conn = pool.acquire();
            char sql[256];
            snprintf(sql, sizeof(sql),
                     "INSERT INTO users(username, password_hash) VALUES('pool_%d', 'hash_%d')", i, i);
            mysql_real_query(conn, sql, strlen(sql));
            pool.release(conn);
            g_pool_done++;
            if (--pending == 0) done_cv.notify_one();
        });
    }

    std::unique_lock<std::mutex> lk(done_mu);
    done_cv.wait(lk, [&]{ return pending.load() == 0; });
}

// ==================== Layer 3: SELECT + INSERT (模拟注册完整路径) ====================

std::atomic<int64_t> g_reg_done{0};

void bench_pool_register(SimpleTP& tp, SimplePool& pool, int total) {
    std::atomic<int> pending{total};
    std::mutex done_mu;
    std::condition_variable done_cv;

    for (int i = 0; i < total; ++i) {
        tp.submit([&pool, &pending, &done_cv, i]() {
            MYSQL* conn = pool.acquire();

            // SELECT 查重
            char sql1[256];
            snprintf(sql1, sizeof(sql1),
                     "SELECT id FROM users WHERE username='reg_%d' LIMIT 1", i);
            mysql_real_query(conn, sql1, strlen(sql1));
            MYSQL_RES* res = mysql_store_result(conn);
            if (res) mysql_free_result(res);

            // INSERT
            char sql2[256];
            snprintf(sql2, sizeof(sql2),
                     "INSERT INTO users(username, password_hash) VALUES('reg_%d', 'hash_%d')", i, i);
            mysql_real_query(conn, sql2, strlen(sql2));

            pool.release(conn);
            g_reg_done++;
            if (--pending == 0) done_cv.notify_one();
        });
    }

    std::unique_lock<std::mutex> lk(done_mu);
    done_cv.wait(lk, [&]{ return pending.load() == 0; });
}

int main(int argc, char* argv[]) {
    const char* host = "127.0.0.1";
    int port = 3306;
    const char* user = "root";
    const char* pass = "wa2355771409";
    const char* db = "chatapp";
    int total = 10000;
    int num_threads = 16;
    int pool_size = 8;

    mysql_library_init(0, nullptr, nullptr);

    std::cout << "============================================================" << std::endl;
    std::cout << " 分层性能测试 (" << total << " 请求)" << std::endl;
    std::cout << "============================================================" << std::endl;
    std::cout << std::endl;

    // === Layer 1: 纯 MySQL 多连接 INSERT ===
    {
        std::cout << "--- Layer 1: 纯 MySQL C API INSERT (" << num_threads << " 连接) ---" << std::endl;
        // 清理
        MYSQL* setup = mysql_init(nullptr);
        mysql_real_connect(setup, host, user, pass, db, port, nullptr, 0);
        mysql_real_query(setup, "TRUNCATE TABLE users", 20);
        mysql_close(setup);

        g_done = 0;
        int per_thread = total / num_threads;
        auto t0 = std::chrono::high_resolution_clock::now();

        std::vector<std::thread> threads;
        for (int i = 0; i < num_threads; ++i)
            threads.emplace_back(bench_raw_mysql, host, port, user, pass, db, i, per_thread);
        for (auto& t : threads) t.join();

        auto t1 = std::chrono::high_resolution_clock::now();
        double dur = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count() / 1000.0;
        std::cout << "  Done: " << g_done << "  Duration: " << std::fixed << std::setprecision(2)
                  << dur << "s  QPS: " << std::setprecision(0) << g_done / dur << std::endl;
        std::cout << std::endl;
    }

    // === Layer 2: 连接池(8) + 线程池(8) INSERT ===
    {
        std::cout << "--- Layer 2: 连接池(" << pool_size << ") + 线程池(" << pool_size << ") INSERT ---" << std::endl;
        MYSQL* setup = mysql_init(nullptr);
        mysql_real_connect(setup, host, user, pass, db, port, nullptr, 0);
        mysql_real_query(setup, "TRUNCATE TABLE users", 20);
        mysql_close(setup);

        SimplePool pool(host, port, user, pass, db, pool_size);
        SimpleTP tp(pool_size);
        g_pool_done = 0;

        auto t0 = std::chrono::high_resolution_clock::now();
        bench_pool_insert(tp, pool, total);
        auto t1 = std::chrono::high_resolution_clock::now();
        double dur = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count() / 1000.0;
        std::cout << "  Done: " << g_pool_done << "  Duration: " << std::fixed << std::setprecision(2)
                  << dur << "s  QPS: " << std::setprecision(0) << g_pool_done / dur << std::endl;
        std::cout << std::endl;
    }

    // === Layer 3: 连接池(8) + 线程池(8) SELECT+INSERT (模拟注册) ===
    {
        std::cout << "--- Layer 3: 连接池(" << pool_size << ") + 线程池(" << pool_size << ") SELECT+INSERT ---" << std::endl;
        MYSQL* setup = mysql_init(nullptr);
        mysql_real_connect(setup, host, user, pass, db, port, nullptr, 0);
        mysql_real_query(setup, "TRUNCATE TABLE users", 20);
        mysql_close(setup);

        SimplePool pool(host, port, user, pass, db, pool_size);
        SimpleTP tp(pool_size);
        g_reg_done = 0;

        auto t0 = std::chrono::high_resolution_clock::now();
        bench_pool_register(tp, pool, total);
        auto t1 = std::chrono::high_resolution_clock::now();
        double dur = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count() / 1000.0;
        std::cout << "  Done: " << g_reg_done << "  Duration: " << std::fixed << std::setprecision(2)
                  << dur << "s  QPS: " << std::setprecision(0) << g_reg_done / dur << std::endl;
        std::cout << std::endl;
    }

    // === Layer 1b: 纯 MySQL 8 连接 INSERT（和连接池数量一致）===
    {
        std::cout << "--- Layer 1b: 纯 MySQL C API INSERT (" << pool_size << " 连接，和连接池一致) ---" << std::endl;
        MYSQL* setup = mysql_init(nullptr);
        mysql_real_connect(setup, host, user, pass, db, port, nullptr, 0);
        mysql_real_query(setup, "TRUNCATE TABLE users", 20);
        mysql_close(setup);

        g_done = 0;
        int per_thread = total / pool_size;
        auto t0 = std::chrono::high_resolution_clock::now();

        std::vector<std::thread> threads;
        for (int i = 0; i < pool_size; ++i)
            threads.emplace_back(bench_raw_mysql, host, port, user, pass, db, 100+i, per_thread);
        for (auto& t : threads) t.join();

        auto t1 = std::chrono::high_resolution_clock::now();
        double dur = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count() / 1000.0;
        std::cout << "  Done: " << g_done << "  Duration: " << std::fixed << std::setprecision(2)
                  << dur << "s  QPS: " << std::setprecision(0) << g_done / dur << std::endl;
    }

    std::cout << std::endl;
    std::cout << "============================================================" << std::endl;
    std::cout << " 对比：HTTP 注册通过协程应用约 5,400 QPS" << std::endl;
    std::cout << "============================================================" << std::endl;

    mysql_library_end();
    return 0;
}
