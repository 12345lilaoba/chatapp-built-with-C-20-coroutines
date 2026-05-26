/**
 * main.cpp - 协程聊天应用主入口文件
 *
 * 本文件是整个协程聊天应用服务器的启动入口，负责：
 *   1. 解析命令行参数配置
 *   2. 初始化各类资源（MySQL连接池、线程池、Redis连接、Worker线程）
 *   3. 创建HTTP服务器socket并监听端口
 *   4. 在主线程中接受客户端连接并分发给Worker线程处理
 *
 * 使用的核心组件：
 *   - coro_net.h:    协程网络层（基于epoll + 协程调度实现高并发）
 *   - async_redis.h: 异步Redis客户端（基于hiredis-async，用于token验证和消息存储）
 *   - async_mysql.h: 异步MySQL客户端（基于线程池，用于用户数据持久化存储）
 *   - http_server.h: HTTP协议解析器（处理HTTP请求和响应）
 *   - chat_app.h:    业务逻辑层（HTTP路由、WebSocket处理、用户认证）
 *   - chat_page.h:   嵌入式前端HTML页面（由chat_app.h自动包含）
 */

#include "chat_app.h"    // 包含聊天应用的业务逻辑和前端页面
#include <csignal>       // 包含信号处理函数（用于忽略SIGPIPE信号）

// ============================================================================
// 线程本地存储变量声明
// ============================================================================

/**
 * 每个Worker线程拥有自己的WorkerRedis实例
 * thread_local关键字确保每个线程都有独立的t_redis副本
 * 这样可以避免多线程竞争同一个Redis连接的问题
 */
thread_local WorkerRedis* t_redis = nullptr;

// ============================================================================
// 全局资源指针声明
// ============================================================================

/**
 * 全局线程池指针
 * 用于执行耗时的阻塞操作（如MySQL查询），避免阻塞协程调度
 */
ThreadPool* g_thread_pool = nullptr;

/**
 * 全局MySQL连接池指针
 * 管理多个MySQL连接，支持并发数据库访问
 */
MySQLPool*  g_mysql_pool  = nullptr;

// ============================================================================
// Worker线程的Redis连接管理
// ============================================================================

/**
 * 存储所有Worker线程的Redis连接实例
 * 使用unique_ptr自动管理内存，确保资源正确释放
 * 每个Worker线程对应一个Redis连接，避免竞争
 */
std::vector<std::unique_ptr<WorkerRedis>> g_worker_redis;

// ============================================================================
// Worker线程入口函数
// ============================================================================

/**
 * Worker线程的入口函数
 * 
 * @param id          Worker线程的ID（从0开始的索引）
 * @param redis_host  Redis服务器的主机地址
 * @param redis_port  Redis服务器的端口号
 * 
 * 功能：
 *   1. 设置线程本地存储的Worker指针（t_worker）
 *   2. 设置线程本地存储的Redis指针（t_redis）
 *   3. 启动Worker的事件循环（run()）
 */
void worker_thread(int id, const std::string& redis_host, int redis_port) {
    // 设置当前线程的Worker指针，用于后续协程调度
    t_worker = g_workers[id].get();
    
    // 设置当前线程的Redis连接指针
    t_redis = g_worker_redis[id].get();
    
    // 启动Worker的事件循环，开始处理epoll事件和协程调度
    t_worker->run();
}

// ============================================================================
// 数据库表初始化函数
// ============================================================================

/**
 * 初始化MySQL数据库表结构
 * 
 * @param pool MySQL连接池指针
 * 
 * 功能：
 *   1. 从连接池获取一个MySQL连接
 *   2. 执行CREATE TABLE语句创建users表（如果不存在）
 *   3. 表结构包含：用户ID、用户名、密码哈希、创建时间
 *   4. 将连接归还到连接池
 */
void init_mysql_tables(MySQLPool* pool) {
    // 从连接池获取一个可用的MySQL连接
    MYSQL* conn = pool->acquire();
    if (!conn) {
        // 连接获取失败，输出错误信息
        std::cerr << "[Init] Failed to get MySQL connection" << std::endl;
        return;
    }

    // 创建users表的SQL语句
    // 使用IF NOT EXISTS避免表已存在时报错
    const char* create_table =
        "CREATE TABLE IF NOT EXISTS users ("
        "  id BIGINT AUTO_INCREMENT PRIMARY KEY,"     // 用户ID，自增主键
        "  username VARCHAR(64) NOT NULL UNIQUE,"     // 用户名，唯一约束
        "  password_hash VARCHAR(64) NOT NULL,"       // 密码哈希值（SHA256）
        "  created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP"  // 创建时间，自动填充
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4";     // 使用InnoDB引擎和utf8mb4编码

    // 执行SQL语句
    if (mysql_real_query(conn, create_table, strlen(create_table)) != 0) {
        // 执行失败，输出错误信息
        std::cerr << "[Init] Create table failed: " << mysql_error(conn) << std::endl;
    } else {
        // 执行成功，输出成功信息
        std::cout << "[Init] MySQL table 'users' ready" << std::endl;
    }
    
    // 将连接归还到连接池，供其他请求使用
    pool->release(conn);
}

// ============================================================================
// 主函数 - 程序入口
// ============================================================================

/**
 * 主函数 - 协程聊天服务器的入口点
 * 
 * @param argc 命令行参数个数
 * @param argv 命令行参数数组
 * @return     程序退出码（0表示正常退出）
 * 
 * 主要流程：
 *   1. 解析命令行参数
 *   2. 初始化信号处理
 *   3. 初始化MySQL库
 *   4. 创建并初始化各类资源池
 *   5. 创建服务器socket并监听
 *   6. 启动Worker线程
 *   7. 主线程accept循环（轮询分发连接到Worker）
 */
int main(int argc, char* argv[]) {
    // ========================================================================
    // 第一步：设置默认配置参数
    // ========================================================================
    
    int port = 8080;                    // HTTP服务器监听端口，默认8080
    int num_workers = 4;                // Worker线程数量，默认4个
    std::string redis_host = "127.0.0.1";  // Redis服务器地址，默认本地
    int redis_port = 6379;              // Redis服务器端口，默认6379
    std::string redis_pass;             // Redis密码，默认空
    std::string mysql_host = "127.0.0.1";  // MySQL服务器地址，默认本地
    int mysql_port = 3306;              // MySQL服务器端口，默认3306
    std::string mysql_user = "root";    // MySQL用户名，默认root
    std::string mysql_pass;             // MySQL密码，默认空
    std::string mysql_db = "chatapp";   // MySQL数据库名，默认chatapp
    int mysql_pool_size = 8;            // MySQL连接池大小，默认8个连接
    int thread_pool_size = 8;           // 线程池大小，默认8个线程

    // ========================================================================
    // 第二步：解析命令行参数
    // ========================================================================
    
    // 遍历所有命令行参数
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        // 根据参数名设置对应的值
        if (arg == "--port" && i + 1 < argc)         port = std::stoi(argv[++i]);
        else if (arg == "--workers" && i + 1 < argc)  num_workers = std::stoi(argv[++i]);
        else if (arg == "--redis-host" && i + 1 < argc) redis_host = argv[++i];
        else if (arg == "--redis-port" && i + 1 < argc) redis_port = std::stoi(argv[++i]);
        else if (arg == "--redis-pass" && i + 1 < argc) redis_pass = argv[++i];
        else if (arg == "--mysql-host" && i + 1 < argc) mysql_host = argv[++i];
        else if (arg == "--mysql-port" && i + 1 < argc) mysql_port = std::stoi(argv[++i]);
        else if (arg == "--mysql-user" && i + 1 < argc) mysql_user = argv[++i];
        else if (arg == "--mysql-pass" && i + 1 < argc) mysql_pass = argv[++i];
        else if (arg == "--mysql-db" && i + 1 < argc)   mysql_db = argv[++i];
        else if (arg == "--mysql-pool" && i + 1 < argc) mysql_pool_size = std::stoi(argv[++i]);
        else if (arg == "--thread-pool" && i + 1 < argc) thread_pool_size = std::stoi(argv[++i]);
        else if (arg == "--help") {
            // 显示帮助信息并退出
            std::cout << "Usage: " << argv[0] << " [options]\n"
                      << "  --port N          HTTP port (default: 8080)\n"
                      << "  --workers N       Worker threads (default: 4)\n"
                      << "  --redis-host H    Redis host (default: 127.0.0.1)\n"
                      << "  --redis-port N    Redis port (default: 6379)\n"
                      << "  --redis-pass P    Redis password (default: empty)\n"
                      << "  --mysql-host H    MySQL host (default: 127.0.0.1)\n"
                      << "  --mysql-port N    MySQL port (default: 3306)\n"
                      << "  --mysql-user U    MySQL user (default: root)\n"
                      << "  --mysql-pass P    MySQL password (default: empty)\n"
                      << "  --mysql-db D      MySQL database (default: chatapp)\n"
                      << std::endl;
            return 0;
        }
    }

    // ========================================================================
    // 第三步：信号处理设置
    // ========================================================================
    
    // 忽略SIGPIPE信号
    // 当向一个已关闭的socket写入数据时，系统会发送SIGPIPE信号
    // 默认行为是终止程序，这里设置为忽略，让write返回EPIPE错误
    signal(SIGPIPE, SIG_IGN);

    // ========================================================================
    // 第四步：初始化MySQL客户端库
    // ========================================================================
    
    // 在多线程环境中使用MySQL前，必须先调用mysql_library_init
    // 这个函数初始化MySQL客户端库的全局状态
    mysql_library_init(0, nullptr, nullptr);

    // ========================================================================
    // 第五步：输出服务器配置信息
    // ========================================================================
    
    std::cout << "========================================" << std::endl;
    std::cout << " Coroutine Chat Server" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "  HTTP Port:    " << port << std::endl;
    std::cout << "  Workers:      " << num_workers << std::endl;
    std::cout << "  Redis:        " << redis_host << ":" << redis_port << std::endl;
    std::cout << "  MySQL:        " << mysql_host << ":" << mysql_port << "/" << mysql_db << std::endl;
    std::cout << "  MySQL Pool:   " << mysql_pool_size << " connections" << std::endl;
    std::cout << "  Thread Pool:  " << thread_pool_size << " threads" << std::endl;
    std::cout << "========================================" << std::endl;

    // ========================================================================
    // 第六步：初始化MySQL连接池
    // ========================================================================
    
    // 创建MySQL连接池实例，管理多个数据库连接
    g_mysql_pool = new MySQLPool(mysql_host, mysql_port, mysql_user, mysql_pass, mysql_db, mysql_pool_size);
    
    // 初始化数据库表结构（创建users表）
    init_mysql_tables(g_mysql_pool);

    // ========================================================================
    // 第七步：初始化线程池
    // ========================================================================
    
    // 创建线程池，用于执行阻塞操作（如MySQL查询）
    // 这样可以避免阻塞协程调度器，提高并发性能
    g_thread_pool = new ThreadPool(thread_pool_size);

    // ========================================================================
    // 第八步：创建Worker线程
    // ========================================================================
    
    // 创建指定数量的Worker实例
    // 每个Worker管理一个epoll实例和协程调度器
    for (int i = 0; i < num_workers; ++i) {
        g_workers.push_back(std::make_unique<Worker>(i));
    }

    // ========================================================================
    // 第九步：为每个Worker创建Redis连接
    // ========================================================================
    
    // 每个Worker线程需要一个独立的Redis异步连接
    // 这是因为hiredis-async不是线程安全的，不能共享连接
    for (int i = 0; i < num_workers; ++i) {
        g_worker_redis.push_back(
            std::make_unique<WorkerRedis>(g_workers[i].get(), redis_host, redis_port, redis_pass));
    }

    // ========================================================================
    // 第十步：创建服务器socket并监听
    // ========================================================================
    
    // 创建监听socket，绑定到指定端口
    int server_fd = create_server(port);
    if (server_fd < 0) {
        // 创建失败，输出错误并退出
        std::cerr << "Failed to create server" << std::endl;
        return 1;
    }

    // ========================================================================
    // 第十一步：启动Worker线程
    // ========================================================================
    
    // 创建线程数组，用于存储所有Worker线程
    std::vector<std::thread> threads;
    
    // 启动所有Worker线程
    // 每个线程运行worker_thread函数，传入线程ID和Redis配置
    for (int i = 0; i < num_workers; ++i) {
        threads.emplace_back(worker_thread, i, redis_host, redis_port);
    }

    // 输出服务器启动信息
    std::cout << "[Server] Listening on http://0.0.0.0:" << port << std::endl;
    std::cout << "[Server] Open browser and visit http://127.0.0.1:" << port << std::endl;

    // ========================================================================
    // 第十二步：主线程accept循环
    // ========================================================================
    
    // next_worker用于轮询选择下一个Worker线程
    // 实现简单的负载均衡
    int next_worker = 0;
    
    // 无限循环，持续接受客户端连接
    while (true) {
        // 接受一个新的客户端连接
        // accept会阻塞，直到有新的连接到达
        int client_fd = accept(server_fd, nullptr, nullptr);
        
        // 检查accept是否成功
        if (client_fd < 0) {
            // 处理非致命错误
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                continue;  // 临时错误，继续循环
            perror("accept");  // 输出错误信息
            continue;  // 继续接受下一个连接
        }

        // 设置客户端socket为非阻塞模式
        // 这是使用epoll边缘触发（ET）模式的必要条件
        set_nonblocking(client_fd);
        
        // 选择当前轮到的Worker线程
        Worker* w = g_workers[next_worker].get();
        WorkerRedis* wr = g_worker_redis[next_worker].get();

        // 将客户端socket添加到Worker的epoll监听集合中
        // 监听可读事件（EPOLLIN），使用边缘触发模式（EPOLLET）
        w->add_client_fd(client_fd, EPOLLIN | EPOLLET);
        
        // 创建协程处理这个HTTP连接
        // handle_http_connection会创建一个协程来处理HTTP请求
        handle_http_connection(client_fd, w, wr, g_thread_pool, g_mysql_pool);

        // 轮询到下一个Worker线程
        // 使用取模运算实现循环
        next_worker = (next_worker + 1) % num_workers;
    }

    // ========================================================================
    // 第十三步：资源清理（实际上不会执行到这里）
    // ========================================================================
    
    // 等待所有Worker线程结束
    for (auto& t : threads) t.join();
    
    // 删除MySQL连接池
    delete g_mysql_pool;
    
    // 删除线程池
    delete g_thread_pool;
    
    // 清理MySQL客户端库
    mysql_library_end();

    return 0;
}
