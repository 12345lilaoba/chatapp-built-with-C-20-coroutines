CXX = g++
CXXFLAGS = -Wall -g -O2 -std=c++20 -I/usr/include/mysql
LDFLAGS = -lhiredis -lpthread -lmysqlclient -lzstd -lssl -lcrypto -lresolv

TARGET = chatserver
BENCH = benchmark
WS_BENCH = ws_bench
SRCS = main.cpp
HEADERS = coro_net.h async_redis.h async_mysql.h http_server.h chat_app.h chat_page.h websocket.h

.PHONY: all clean help

BENCH_LAYERS = bench_layers

all: $(TARGET) $(BENCH) $(WS_BENCH) $(BENCH_LAYERS)

$(TARGET): $(SRCS) $(HEADERS)
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS) $(LDFLAGS)

$(BENCH): benchmark.cpp
	$(CXX) -Wall -g -O2 -std=c++20 -o $@ $< -lpthread

$(WS_BENCH): ws_bench.cpp
	$(CXX) -Wall -g -O2 -std=c++20 -o $@ $< -lpthread

$(BENCH_LAYERS): bench_layers.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGET) $(BENCH) $(WS_BENCH)

help:
	@echo "协程聊天服务器"
	@echo ""
	@echo "编译: make"
	@echo "清理: make clean"
	@echo ""
	@echo "运行: ./chatserver [options]"
	@echo "  --port N          HTTP 端口 (默认: 8080)"
	@echo "  --workers N       Worker 线程数 (默认: 4)"
	@echo "  --redis-host H    Redis 地址 (默认: 127.0.0.1)"
	@echo "  --redis-port N    Redis 端口 (默认: 6379)"
	@echo "  --redis-pass P    Redis 密码 (默认: 空)"
	@echo "  --mysql-host H    MySQL 地址 (默认: 127.0.0.1)"
	@echo "  --mysql-port N    MySQL 端口 (默认: 3306)"
	@echo "  --mysql-user U    MySQL 用户 (默认: root)"
	@echo "  --mysql-pass P    MySQL 密码 (默认: 空)"
	@echo "  --mysql-db D      MySQL 数据库 (默认: chatapp)"
	@echo ""
	@echo "依赖:"
	@echo "  sudo apt install libhiredis-dev libmysqlclient-dev redis-server mysql-server"
