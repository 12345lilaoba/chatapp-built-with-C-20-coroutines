#!/bin/bash
set -e

PORT=9090
MYSQL_USER="root"
MYSQL_PASS="wa2355771409"
BENCH_THREADS=16
BENCH_REQ=625
DIR="$(cd "$(dirname "$0")" && pwd)"

echo "============================================================"
echo " 参数调优对比压测"
echo " 每轮：${BENCH_THREADS}线程 x ${BENCH_REQ}请求 = $((BENCH_THREADS * BENCH_REQ)) 请求"
echo "============================================================"
echo ""

printf "%-10s %-12s %-13s | %-12s %-12s %-12s %-12s\n" \
    "Workers" "MySQL Pool" "Thread Pool" "Reg QPS" "Reg Lat(ms)" "Login QPS" "Login Lat(ms)"
echo "----------+------------+-------------+-------------+-------------+-------------+------------"

run_one() {
    local workers=$1
    local mysql_pool=$2
    local thread_pool=$3

    # 清理数据
    mysql -u $MYSQL_USER -p$MYSQL_PASS chatapp -e "TRUNCATE TABLE users" 2>/dev/null
    redis-cli -a 123321 FLUSHDB > /dev/null 2>&1

    # 启动服务器
    "$DIR/chatserver" --port $PORT --workers $workers \
        --mysql-user $MYSQL_USER --mysql-pass $MYSQL_PASS \
        --mysql-pool $mysql_pool --thread-pool $thread_pool \
        > /dev/null 2>&1 &
    local spid=$!
    sleep 0.8

    if ! kill -0 $spid 2>/dev/null; then
        echo "Server failed: w=$workers mp=$mysql_pool tp=$thread_pool"
        return
    fi

    # 注册压测
    local reg_output
    reg_output=$("$DIR/benchmark" -h 127.0.0.1 -p $PORT -t $BENCH_THREADS -n $BENCH_REQ -m register 2>&1)
    local reg_qps
    reg_qps=$(echo "$reg_output" | grep "QPS:" | awk '{print $2}')
    local reg_lat
    reg_lat=$(echo "$reg_output" | grep "Avg Latency:" | awk '{print $3}')

    # 登录压测
    local login_output
    login_output=$("$DIR/benchmark" -h 127.0.0.1 -p $PORT -t $BENCH_THREADS -n $BENCH_REQ -m login 2>&1)
    local login_qps
    login_qps=$(echo "$login_output" | grep "QPS:" | awk '{print $2}')
    local login_lat
    login_lat=$(echo "$login_output" | grep "Avg Latency:" | awk '{print $3}')

    # 关闭服务器
    kill $spid 2>/dev/null
    wait $spid 2>/dev/null || true
    sleep 0.3

    printf "%-10s %-12s %-13s | %-12s %-12s %-12s %-12s\n" \
        "$workers" "$mysql_pool" "$thread_pool" "$reg_qps" "$reg_lat" "$login_qps" "$login_lat"
}

run_one 2 4 4
run_one 2 8 8
run_one 2 16 16
run_one 4 4 4
run_one 4 8 8
run_one 4 16 16
run_one 4 32 32
run_one 8 8 8
run_one 8 16 16
run_one 8 32 32

echo ""
echo "============================================================"
echo " 压测完成"
echo "============================================================"
