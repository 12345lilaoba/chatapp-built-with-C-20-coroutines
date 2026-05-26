#!/bin/bash
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"

pkill -f chatserver 2>/dev/null || true
sleep 0.5

echo "============================================================"
echo " 连接池大小对 HTTP 注册 QPS 的影响"
echo " 16线程 x 4连接/线程, 10000请求, Keep-Alive"
echo "============================================================"
echo ""

printf "%-12s %-12s | %-10s %-10s %-10s\n" "MySQL Pool" "Thread Pool" "QPS" "Lat(ms)" "Failed"
echo "------------+------------+----------+----------+---------"

for pool_size in 4 8 16 32; do
    mysql -u root -pwa2355771409 chatapp -e "TRUNCATE TABLE users" 2>/dev/null
    redis-cli -a 123321 FLUSHDB > /dev/null 2>&1

    "$DIR/chatserver" --port 9090 --workers 4 --mysql-user root --mysql-pass wa2355771409 \
        --mysql-pool $pool_size --thread-pool $pool_size > /dev/null 2>&1 &
    SERVER_PID=$!
    sleep 0.8

    if ! kill -0 $SERVER_PID 2>/dev/null; then
        echo "Server failed with pool=$pool_size"
        continue
    fi

    output=$("$DIR/benchmark" -p 9090 -t 16 -c 4 -n 10000 -m register -k 2>&1)
    qps=$(echo "$output" | grep "QPS:" | awk '{print $2}')
    lat=$(echo "$output" | grep "Avg Latency:" | awk '{print $3}')
    failed=$(echo "$output" | grep "Failed:" | awk '{print $2}')

    printf "%-12s %-12s | %-10s %-10s %-10s\n" "$pool_size" "$pool_size" "$qps" "$lat" "$failed"

    kill $SERVER_PID 2>/dev/null
    wait $SERVER_PID 2>/dev/null || true
    sleep 0.3
done

echo ""
echo "============================================================"
echo " 测试完成"
echo "============================================================"
