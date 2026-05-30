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
BENCH_THREADS="${BENCH_THREADS:-16}"
BENCH_CONN_PER_THREAD="${BENCH_CONN_PER_THREAD:-4}"
BENCH_REQ="${BENCH_REQ:-10000}"
KEEP_ALIVE="${KEEP_ALIVE:-1}"
MYSQL_HOST="${MYSQL_HOST:-127.0.0.1}"
MYSQL_PORT="${MYSQL_PORT:-3306}"
MYSQL_USER="${MYSQL_USER:-root}"
MYSQL_PASS="${MYSQL_PASS:-}"
MYSQL_DB="${MYSQL_DB:-chatapp}"
REDIS_HOST="${REDIS_HOST:-127.0.0.1}"
REDIS_PORT="${REDIS_PORT:-6379}"
REDIS_PASS="${REDIS_PASS:-}"

CASES=(
  "2 4 4"
  "2 8 8"
  "2 16 16"
  "4 4 4"
  "4 8 8"
  "4 16 16"
  "4 32 32"
  "8 8 8"
  "8 16 16"
  "8 32 32"
)

SERVER_PID=""
cleanup_server() {
  if [[ -n "${SERVER_PID}" ]] && kill -0 "${SERVER_PID}" 2>/dev/null; then
    kill "${SERVER_PID}" 2>/dev/null || true
    wait "${SERVER_PID}" 2>/dev/null || true
  fi
}
trap cleanup_server EXIT

pkill -f "${DIR}/chatserver" 2>/dev/null || true
sleep 0.5

make benchmark >/dev/null
make chatserver >/dev/null

bench_args=(-h 127.0.0.1 -p "${PORT}" -t "${BENCH_THREADS}" -c "${BENCH_CONN_PER_THREAD}" -n "${BENCH_REQ}")
if [[ "${KEEP_ALIVE}" == "1" ]]; then
  bench_args+=(-k)
fi

extract_metric() {
  local output="$1"
  local label="$2"
  echo "${output}" | awk -v label="${label}" '$1 == label {print $2; exit}'
}

run_one() {
  local workers="$1"
  local mysql_pool="$2"
  local thread_pool="$3"

  ./reset_bench_data.sh >/dev/null

  "${DIR}/chatserver"     --port "${PORT}"     --workers "${workers}"     --redis-host "${REDIS_HOST}"     --redis-port "${REDIS_PORT}"     --redis-pass "${REDIS_PASS}"     --mysql-host "${MYSQL_HOST}"     --mysql-port "${MYSQL_PORT}"     --mysql-user "${MYSQL_USER}"     --mysql-pass "${MYSQL_PASS}"     --mysql-db "${MYSQL_DB}"     --mysql-pool "${mysql_pool}"     --thread-pool "${thread_pool}" >/tmp/chatapp_run_bench_${workers}_${mysql_pool}_${thread_pool}.log 2>&1 &
  SERVER_PID=$!
  sleep 1

  if ! kill -0 "${SERVER_PID}" 2>/dev/null; then
    printf "%-8s %-10s %-11s | %-9s %-10s %-8s | %-9s %-10s %-8s\n"       "${workers}" "${mysql_pool}" "${thread_pool}" "SERVER" "FAILED" "-" "SERVER" "FAILED" "-"
    SERVER_PID=""
    return
  fi

  local reg_output login_output
  reg_output=$("${DIR}/benchmark" "${bench_args[@]}" -m register 2>&1)
  login_output=$("${DIR}/benchmark" "${bench_args[@]}" -m login 2>&1)

  local reg_qps reg_lat reg_failed login_qps login_lat login_failed
  reg_qps=$(echo "${reg_output}" | awk '/QPS:/ {print $2; exit}')
  reg_lat=$(echo "${reg_output}" | awk '/Avg Latency:/ {print $3; exit}')
  reg_failed=$(echo "${reg_output}" | awk '/Failed:/ {print $2; exit}')
  login_qps=$(echo "${login_output}" | awk '/QPS:/ {print $2; exit}')
  login_lat=$(echo "${login_output}" | awk '/Avg Latency:/ {print $3; exit}')
  login_failed=$(echo "${login_output}" | awk '/Failed:/ {print $2; exit}')

  cleanup_server
  SERVER_PID=""
  sleep 0.3

  printf "%-8s %-10s %-11s | %-9s %-10s %-8s | %-9s %-10s %-8s\n"     "${workers}" "${mysql_pool}" "${thread_pool}"     "${reg_qps:-NA}" "${reg_lat:-NA}" "${reg_failed:-NA}"     "${login_qps:-NA}" "${login_lat:-NA}" "${login_failed:-NA}"
}

echo "================================================================================"
echo " 综合参数压测"
echo " 每轮：${BENCH_THREADS}线程 x ${BENCH_CONN_PER_THREAD}连接/线程, ${BENCH_REQ}请求, Keep-Alive=${KEEP_ALIVE}"
echo "================================================================================"
echo ""
printf "%-8s %-10s %-11s | %-9s %-10s %-8s | %-9s %-10s %-8s\n"   "Workers" "MySQLPool" "ThreadPool" "RegQPS" "RegLat" "RegFail" "LoginQPS" "LoginLat" "LogFail"
echo "--------+----------+-----------+-----------+----------+--------+-----------+----------+--------"

for case in "${CASES[@]}"; do
  # shellcheck disable=SC2086
  run_one ${case}
done

echo ""
echo "================================================================================"
echo " 压测完成"
echo "================================================================================"
