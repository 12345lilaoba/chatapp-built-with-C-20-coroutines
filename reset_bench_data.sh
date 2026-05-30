#!/usr/bin/env bash
set -euo pipefail

# Put local secrets in .bench_env (ignored by git), for example:
#   MYSQL_USER=root
#   MYSQL_PASS=your_mysql_password
#   MYSQL_DB=chatapp
#   REDIS_PASS=your_redis_password
#   REDIS_DB=0

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ENV_FILE="${SCRIPT_DIR}/.bench_env"

if [[ -f "${ENV_FILE}" ]]; then
  # shellcheck disable=SC1090
  source "${ENV_FILE}"
fi

MYSQL_HOST="${MYSQL_HOST:-127.0.0.1}"
MYSQL_PORT="${MYSQL_PORT:-3306}"
MYSQL_USER="${MYSQL_USER:-root}"
MYSQL_PASS="${MYSQL_PASS:-}"
MYSQL_DB="${MYSQL_DB:-chatapp}"

REDIS_HOST="${REDIS_HOST:-127.0.0.1}"
REDIS_PORT="${REDIS_PORT:-6379}"
REDIS_PASS="${REDIS_PASS:-}"
REDIS_DB="${REDIS_DB:-0}"

MYSQL_BASE_ARGS=(--protocol=TCP -h "${MYSQL_HOST}" -P "${MYSQL_PORT}" -u "${MYSQL_USER}" "${MYSQL_DB}")
REDIS_BASE_ARGS=(-h "${REDIS_HOST}" -p "${REDIS_PORT}" -n "${REDIS_DB}")
REDIS_CMD=(redis-cli "${REDIS_BASE_ARGS[@]}")
if [[ -n "${REDIS_PASS}" ]]; then
  REDIS_CMD=(env REDISCLI_AUTH="${REDIS_PASS}" redis-cli "${REDIS_BASE_ARGS[@]}")
fi

echo "[1/3] Checking MySQL auth: ${MYSQL_USER}@${MYSQL_HOST}:${MYSQL_PORT}/${MYSQL_DB}"
if [[ -n "${MYSQL_PASS}" ]]; then
  if ! MYSQL_PWD="${MYSQL_PASS}" mysql "${MYSQL_BASE_ARGS[@]}" -e "SELECT 1;" >/dev/null 2>&1; then
    echo "MySQL auth failed. Check .bench_env or MYSQL_* env vars." >&2
    exit 1
  fi
else
  if ! mysql "${MYSQL_BASE_ARGS[@]}" -e "SELECT 1;" >/dev/null 2>&1; then
    echo "MySQL auth failed (no password). Put MYSQL_PASS in .bench_env or export it." >&2
    exit 1
  fi
fi

echo "[2/3] Clearing MySQL benchmark tables"
if [[ -n "${MYSQL_PASS}" ]]; then
  MYSQL_PWD="${MYSQL_PASS}" mysql "${MYSQL_BASE_ARGS[@]}" -e "SET FOREIGN_KEY_CHECKS=0; TRUNCATE TABLE room_members; TRUNCATE TABLE rooms; TRUNCATE TABLE users; SET FOREIGN_KEY_CHECKS=1; INSERT IGNORE INTO rooms(room_id,name,description,owner_username) VALUES('general','General Chat','Welcome to the chatroom','');"
else
  mysql "${MYSQL_BASE_ARGS[@]}" -e "SET FOREIGN_KEY_CHECKS=0; TRUNCATE TABLE room_members; TRUNCATE TABLE rooms; TRUNCATE TABLE users; SET FOREIGN_KEY_CHECKS=1; INSERT IGNORE INTO rooms(room_id,name,description,owner_username) VALUES('general','General Chat','Welcome to the chatroom','');"
fi

echo "[3/3] Clearing Redis benchmark keys"
"${REDIS_CMD[@]}" --scan --pattern 'token:*' | xargs -r "${REDIS_CMD[@]}" DEL >/dev/null
"${REDIS_CMD[@]}" --scan --pattern 'chatroom:*' | xargs -r "${REDIS_CMD[@]}" DEL >/dev/null

echo "Done."
