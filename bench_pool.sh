#!/bin/bash
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$DIR"

if [[ -f .bench_env ]]; then
  set -a
  source .bench_env
  set +a
fi

PORT="${PORT:-9090}"
WORKERS="${WORKERS:-4}"
BENCH_THREADS="${BENCH_THREADS:-16}"
BENCH_CONN_PER_THREAD="${BENCH_CONN_PER_THREAD:-4}"
BENCH_REQ="${BENCH_REQ:-10000}"
POOL_SIZES="${POOL_SIZES:-4 8 16 32}"
MYSQL_HOST="${MYSQL_HOST:-127.0.0.1}"
MYSQL_PORT="${MYSQL_PORT:-3306}"
MYSQL_USER="${MYSQL_USER:-root}"
MYSQL_PASS="${MYSQL_PASS:-}"
MYSQL_DB="${MYSQL_DB:-chatapp}"
REDIS_HOST="${REDIS_HOST:-127.0.0.1}"
REDIS_PORT="${REDIS_PORT:-6379}"
REDIS_PASS="${REDIS_PASS:-}"

SERVER_PID=""
cleanup() {
  if [[ -n "${SERVER_PID}" ]] && kill -0 "${SERVER_PID}" 2>/dev/null; then
    kill "${SERVER_PID}" 2>/dev/null || true
    wait "${SERVER_PID}" 2>/dev/null || true
  fi
}
trap cleanup EXIT

pkill -f "${DIR}/chatserver" 2>/dev/null || true
sleep 0.5

make benchmark >/dev/null
make chatserver >/dev/null

echo "============================================================"
echo " 连接池大小对 HTTP 注册 QPS 的影响"
echo " ${BENCH_THREADS}线程 x ${BENCH_CONN_PER_THREAD}连接/线程, ${BENCH_REQ}请求, Keep-Alive"
echo "============================================================"
echo ""

printf "%-12s %-12s | %-10s %-10s %-10s\n" "MySQL Pool" "Thread Pool" "QPS" "Lat(ms)" "Failed"
echo "------------+------------+----------+----------+---------"

for pool_size in ${POOL_SIZES}; do
  ./reset_bench_data.sh >/dev/null

  "${DIR}/chatserver"     --port "${PORT}"     --workers "${WORKERS}"     --redis-host "${REDIS_HOST}"     --redis-port "${REDIS_PORT}"     --redis-pass "${REDIS_PASS}"     --mysql-host "${MYSQL_HOST}"     --mysql-port "${MYSQL_PORT}"     --mysql-user "${MYSQL_USER}"     --mysql-pass "${MYSQL_PASS}"     --mysql-db "${MYSQL_DB}"     --mysql-pool "${pool_size}"     --thread-pool "${pool_size}" >/tmp/chatapp_bench_pool_${pool_size}.log 2>&1 &
  SERVER_PID=$!
  sleep 1

  if ! kill -0 "${SERVER_PID}" 2>/dev/null; then
    echo "Server failed with pool=${pool_size}; see /tmp/chatapp_bench_pool_${pool_size}.log"
    SERVER_PID=""
    continue
  fi

  output=$("${DIR}/benchmark" -p "${PORT}" -t "${BENCH_THREADS}" -c "${BENCH_CONN_PER_THREAD}" -n "${BENCH_REQ}" -m register -k 2>&1)
  qps=$(echo "${output}" | awk '/QPS:/ {print $2; exit}')
  lat=$(echo "${output}" | awk '/Avg Latency:/ {print $3; exit}')
  failed=$(echo "${output}" | awk '/Failed:/ {print $2; exit}')

  printf "%-12s %-12s | %-10s %-10s %-10s\n" "${pool_size}" "${pool_size}" "${qps:-NA}" "${lat:-NA}" "${failed:-NA}"

  cleanup
  SERVER_PID=""
  sleep 0.3
done

echo ""
echo "============================================================"
echo " 测试完成"
echo "============================================================"
