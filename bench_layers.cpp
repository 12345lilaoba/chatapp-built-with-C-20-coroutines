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
#include <cstdlib>
#include <fstream>
// #include <sstream>

std::atomic<int64_t> g_done{0};
std::atomic<int64_t> g_failed{0};
std::atomic<int64_t> g_inserted{0};

struct BenchConfig {
    std::string host = "127.0.0.1";
    int port = 3306;
    std::string user = "root";
    std::string pass;
    std::string db = "chatapp";
    int total = 10000;
    int num_threads = 16;
    int pool_size = 8;
};

static std::string trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
    return s.substr(i);
}

static void apply_env_value(BenchConfig& cfg, const std::string& key, const std::string& value) {
    if (key == "MYSQL_HOST") cfg.host = value;
    else if (key == "MYSQL_PORT") cfg.port = std::stoi(value);
    else if (key == "MYSQL_USER") cfg.user = value;
    else if (key == "MYSQL_PASS") cfg.pass = value;
    else if (key == "MYSQL_DB") cfg.db = value;
}

static void load_bench_env(BenchConfig& cfg, const std::string& path = ".bench_env") {
    std::ifstream in(path);
    if (!in) return;
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(line.substr(0, eq));
        std::string value = trim(line.substr(eq + 1));
        if (value.size() >= 2 && ((value.front() == '\'' && value.back() == '\'') ||
                                  (value.front() == '"' && value.back() == '"'))) {
            value = value.substr(1, value.size() - 2);
        }
        apply_env_value(cfg, key, value);
    }
}

static void load_process_env(BenchConfig& cfg) {
    auto get = [](const char* name) -> const char* { return std::getenv(name); };
    if (auto v = get("MYSQL_HOST")) cfg.host = v;
    if (auto v = get("MYSQL_PORT")) cfg.port = std::stoi(v);
    if (auto v = get("MYSQL_USER")) cfg.user = v;
    if (auto v = get("MYSQL_PASS")) cfg.pass = v;
    if (auto v = get("MYSQL_DB")) cfg.db = v;
}

static void parse_args(BenchConfig& cfg, int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "-n" || arg == "--requests") && i + 1 < argc) cfg.total = std::stoi(argv[++i]);
        else if ((arg == "-t" || arg == "--threads") && i + 1 < argc) cfg.num_threads = std::stoi(argv[++i]);
        else if ((arg == "-p" || arg == "--pool") && i + 1 < argc) cfg.pool_size = std::stoi(argv[++i]);
        else if (arg == "--mysql-host" && i + 1 < argc) cfg.host = argv[++i];
        else if (arg == "--mysql-port" && i + 1 < argc) cfg.port = std::stoi(argv[++i]);
        else if (arg == "--mysql-user" && i + 1 < argc) cfg.user = argv[++i];
        else if (arg == "--mysql-pass" && i + 1 < argc) cfg.pass = argv[++i];
        else if (arg == "--mysql-db" && i + 1 < argc) cfg.db = argv[++i];
        else if (arg == "--help") {
            std::cout << "Usage: ./bench_layers [-n requests] [-t threads] [--pool size] [--mysql-* ...]\n";
            std::exit(0);
        }
    }
}

static MYSQL* connect_mysql(const BenchConfig& cfg) {
    MYSQL* conn = mysql_init(nullptr);
    unsigned int timeout = 5;
    mysql_options(conn, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
    mysql_options(conn, MYSQL_SET_CHARSET_NAME, "utf8mb4");
    if (!mysql_real_connect(conn, cfg.host.c_str(), cfg.user.c_str(), cfg.pass.c_str(),
                            cfg.db.c_str(), cfg.port, nullptr, 0)) {
        std::cerr << "Connect failed: " << mysql_error(conn) << std::endl;
        mysql_close(conn);
        return nullptr;
    }
    return conn;
}

static bool exec_sql(MYSQL* conn, const char* sql) {
    return mysql_real_query(conn, sql, strlen(sql)) == 0;
}

static void reset_users(const BenchConfig& cfg) {
    MYSQL* conn = connect_mysql(cfg);
    if (!conn) std::exit(1);

    const char* sqls[] = {
        "SET FOREIGN_KEY_CHECKS=0",
        "TRUNCATE TABLE room_members",
        "TRUNCATE TABLE users",
        "SET FOREIGN_KEY_CHECKS=1"
    };
    for (const char* sql : sqls) {
        if (!exec_sql(conn, sql)) {
            std::cerr << "Reset failed: " << mysql_error(conn) << std::endl;
            mysql_close(conn);
            std::exit(1);
        }
    }
    mysql_close(conn);
}


// ==================== Layer 1: 纯 MySQL C API INSERT ====================

void bench_raw_mysql_range(const BenchConfig& cfg, int start_id, int end_id) {
    MYSQL* conn = connect_mysql(cfg);
    if (!conn) return;

    for (int i = start_id; i < end_id; ++i) {
        char sql[256];
        snprintf(sql, sizeof(sql),
                 "INSERT IGNORE INTO users(username, password_hash) VALUES('raw_%d', 'hash_%d')",
                 i, i);
        if (mysql_real_query(conn, sql, strlen(sql)) != 0) {
            g_failed++;
        } else {
            g_inserted += mysql_affected_rows(conn);
        }
        g_done++;
    }
    mysql_close(conn);
}

// ==================== Layer 2: 连接池 + 线程池 INSERT ====================

class SimplePool {
public:
    SimplePool(const BenchConfig& cfg, int size) {
        for (int i = 0; i < size; ++i) {
            MYSQL* c = mysql_init(nullptr);
            unsigned int timeout = 5;
            mysql_options(c, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
            mysql_options(c, MYSQL_SET_CHARSET_NAME, "utf8mb4");
            if (mysql_real_connect(c, cfg.host.c_str(), cfg.user.c_str(), cfg.pass.c_str(), cfg.db.c_str(), cfg.port, nullptr, 0))
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
std::atomic<int64_t> g_pool_failed{0};
std::atomic<int64_t> g_pool_inserted{0};

void bench_pool_direct_insert(SimplePool& pool, int total, int workers) {
    std::atomic<int> next{0};
    std::vector<std::thread> threads;
    threads.reserve(workers);

    for (int t = 0; t < workers; ++t) {
        threads.emplace_back([&]() {
            while (true) {
                int i = next.fetch_add(1, std::memory_order_relaxed);
                if (i >= total) break;

                MYSQL* conn = pool.acquire();
                char sql[256];
                snprintf(sql, sizeof(sql),
                         "INSERT IGNORE INTO users(username, password_hash) VALUES('pool_direct_%d', 'hash_%d')", i, i);
                if (mysql_real_query(conn, sql, strlen(sql)) != 0) {
                    g_pool_failed++;
                } else {
                    g_pool_inserted += mysql_affected_rows(conn);
                }
                pool.release(conn);
                g_pool_done++;
            }
        });
    }

    for (auto& t : threads) t.join();
}

void bench_pool_insert(SimpleTP& tp, SimplePool& pool, int total) {
    std::atomic<int> pending{total};
    std::mutex done_mu;
    std::condition_variable done_cv;

    for (int i = 0; i < total; ++i) {
        tp.submit([&pool, &pending, &done_cv, i]() {
            MYSQL* conn = pool.acquire();
            char sql[256];
            snprintf(sql, sizeof(sql),
                     "INSERT IGNORE INTO users(username, password_hash) VALUES('pool_%d', 'hash_%d')", i, i);
            if (mysql_real_query(conn, sql, strlen(sql)) != 0) {
                g_pool_failed++;
            } else {
                g_pool_inserted += mysql_affected_rows(conn);
            }
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
std::atomic<int64_t> g_reg_failed{0};
std::atomic<int64_t> g_reg_inserted{0};

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
            if (mysql_real_query(conn, sql1, strlen(sql1)) != 0) {
                g_reg_failed++;
                pool.release(conn);
                g_reg_done++;
                if (--pending == 0) done_cv.notify_one();
                return;
            }
            MYSQL_RES* res = mysql_store_result(conn);
            if (res) mysql_free_result(res);
            else if (mysql_field_count(conn) != 0) {
                g_reg_failed++;
                pool.release(conn);
                g_reg_done++;
                if (--pending == 0) done_cv.notify_one();
                return;
            }

            // INSERT
            char sql2[256];
            snprintf(sql2, sizeof(sql2),
                     "INSERT IGNORE INTO users(username, password_hash) VALUES('reg_%d', 'hash_%d')", i, i);
            if (mysql_real_query(conn, sql2, strlen(sql2)) != 0) {
                g_reg_failed++;
            } else {
                g_reg_inserted += mysql_affected_rows(conn);
            }

            pool.release(conn);
            g_reg_done++;
            if (--pending == 0) done_cv.notify_one();
        });
    }

    std::unique_lock<std::mutex> lk(done_mu);
    done_cv.wait(lk, [&]{ return pending.load() == 0; });
}

int main(int argc, char* argv[]) {
    BenchConfig cfg;
    load_bench_env(cfg);
    load_process_env(cfg);
    parse_args(cfg, argc, argv);

    int total = cfg.total;
    int num_threads = cfg.num_threads;
    int pool_size = cfg.pool_size;

    mysql_library_init(0, nullptr, nullptr);

    std::cout << "============================================================" << std::endl;
    std::cout << " 分层性能测试 (" << total << " 请求)" << std::endl;
    std::cout << " MySQL: " << cfg.user << "@" << cfg.host << ":" << cfg.port << "/" << cfg.db << std::endl;
    std::cout << "============================================================" << std::endl;
    std::cout << std::endl;

    // === Layer 1: 纯 MySQL 多连接 INSERT ===
    {
        std::cout << "--- Layer 1: 纯 MySQL C API INSERT (" << num_threads << " 连接) ---" << std::endl;
        reset_users(cfg);

        g_done = 0;
        g_failed = 0;
        g_inserted = 0;
        auto t0 = std::chrono::high_resolution_clock::now();

        std::vector<std::thread> threads;
        int base = 0;
        for (int i = 0; i < num_threads; ++i) {
            int next = total * (i + 1) / num_threads;
            threads.emplace_back(bench_raw_mysql_range, std::cref(cfg), base, next);
            base = next;
        }
        for (auto& t : threads) t.join();

        auto t1 = std::chrono::high_resolution_clock::now();
        double dur = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count() / 1000.0;
        std::cout << "  Done: " << g_done << "  Duration: " << std::fixed << std::setprecision(2)
                  << dur << "s  QPS: " << std::setprecision(0) << g_done / dur << std::endl;
        std::cout << "  Inserted: " << g_inserted << "  Failed: " << g_failed << std::endl;
        std::cout << std::endl;
    }

    // === Layer 2a: 连接池 + worker循环 INSERT ===
    {
        std::cout << "--- Layer 2a: 连接池(" << pool_size << ") + worker循环 INSERT ---" << std::endl;
        reset_users(cfg);

        SimplePool pool(cfg, pool_size);
        g_pool_done = 0;
        g_pool_failed = 0;
        g_pool_inserted = 0;

        auto t0 = std::chrono::high_resolution_clock::now();
        bench_pool_direct_insert(pool, total, pool_size);
        auto t1 = std::chrono::high_resolution_clock::now();
        double dur = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count() / 1000.0;
        std::cout << "  Done: " << g_pool_done << "  Duration: " << std::fixed << std::setprecision(2)
                  << dur << "s  QPS: " << std::setprecision(0) << g_pool_done / dur << std::endl;
        std::cout << "  Inserted: " << g_pool_inserted << "  Failed: " << g_pool_failed << std::endl;
        std::cout << std::endl;
    }

    // === Layer 2b: 连接池 + 线程池任务队列 INSERT ===
    {
        std::cout << "--- Layer 2b: 连接池(" << pool_size << ") + 线程池任务队列(" << pool_size << ") INSERT ---" << std::endl;
        reset_users(cfg);

        SimplePool pool(cfg, pool_size);
        SimpleTP tp(pool_size);
        g_pool_done = 0;
        g_pool_failed = 0;
        g_pool_inserted = 0;

        auto t0 = std::chrono::high_resolution_clock::now();
        bench_pool_insert(tp, pool, total);
        auto t1 = std::chrono::high_resolution_clock::now();
        double dur = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count() / 1000.0;
        std::cout << "  Done: " << g_pool_done << "  Duration: " << std::fixed << std::setprecision(2)
                  << dur << "s  QPS: " << std::setprecision(0) << g_pool_done / dur << std::endl;
        std::cout << "  Inserted: " << g_pool_inserted << "  Failed: " << g_pool_failed << std::endl;
        std::cout << std::endl;
    }

    // === Layer 3: 连接池(8) + 线程池(8) SELECT+INSERT (模拟注册) ===
    {
        std::cout << "--- Layer 3: 连接池(" << pool_size << ") + 线程池任务队列(" << pool_size << ") SELECT+INSERT ---" << std::endl;
        reset_users(cfg);

        SimplePool pool(cfg, pool_size);
        SimpleTP tp(pool_size);
        g_reg_done = 0;
        g_reg_failed = 0;
        g_reg_inserted = 0;

        auto t0 = std::chrono::high_resolution_clock::now();
        bench_pool_register(tp, pool, total);
        auto t1 = std::chrono::high_resolution_clock::now();
        double dur = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count() / 1000.0;
        std::cout << "  Done: " << g_reg_done << "  Duration: " << std::fixed << std::setprecision(2)
                  << dur << "s  QPS: " << std::setprecision(0) << g_reg_done / dur << std::endl;
        std::cout << "  Inserted: " << g_reg_inserted << "  Failed: " << g_reg_failed << std::endl;
        std::cout << std::endl;
    }

    // === Layer 1b: 纯 MySQL 8 连接 INSERT（和连接池数量一致）===
    {
        std::cout << "--- Layer 1b: 纯 MySQL C API INSERT (" << pool_size << " 连接，和连接池一致) ---" << std::endl;
        reset_users(cfg);

        g_done = 0;
        g_failed = 0;
        g_inserted = 0;
        auto t0 = std::chrono::high_resolution_clock::now();

        std::vector<std::thread> threads;
        int base = 0;
        for (int i = 0; i < pool_size; ++i) {
            int next = total * (i + 1) / pool_size;
            threads.emplace_back(bench_raw_mysql_range, std::cref(cfg), base, next);
            base = next;
        }
        for (auto& t : threads) t.join();

        auto t1 = std::chrono::high_resolution_clock::now();
        double dur = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count() / 1000.0;
        std::cout << "  Done: " << g_done << "  Duration: " << std::fixed << std::setprecision(2)
                  << dur << "s  QPS: " << std::setprecision(0) << g_done / dur << std::endl;
        std::cout << "  Inserted: " << g_inserted << "  Failed: " << g_failed << std::endl;
    }

    std::cout << std::endl;
    std::cout << "============================================================" << std::endl;
    std::cout << " 提示：请将本结果与 ./benchmark -m register 的实际结果对比" << std::endl;
    std::cout << "============================================================" << std::endl;

    mysql_library_end();
    return 0;
}
