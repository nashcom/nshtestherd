#!/bin/bash
#
# integration.sh - end-to-end checks of the HTTP layer with curl
#
# Usage: tests/integration.sh [path-to-nshtestherd]
# Environment: HERD_TEST_PORT (default 18788). Refuses to run if the port is in use.

BIN="${1:-./nshtestherd}"
PORT="${HERD_TEST_PORT:-18788}"
BASE="http://127.0.0.1:$PORT"
FAILS=0
CHECKS=0
LINE="$(printf '%90s' '' | tr ' ' '-')"


header()
{
  echo ""
  echo "$LINE"
  echo "$1"
  echo "$LINE"
  echo ""
}


check()
{
  CHECKS=$((CHECKS + 1))

  if [ "$2" != "$3" ]
  then
    FAILS=$((FAILS + 1))
    echo "[ FAIL ] $1 (expected '$2', got '$3')"
  else
    echo "[ OK ]   $1"
  fi
}


code()
{
  curl -s -o /dev/null -w '%{http_code}' "$@"
}


if (exec 3<>"/dev/tcp/127.0.0.1/$PORT") 2>/dev/null
then
  echo "Port $PORT is in use - set HERD_TEST_PORT to a free port"
  exit 2
fi

header "Start server on port $PORT"

"$BIN" --port "$PORT" --max-csv-bytes 4096 --timeout 3 &
PID=$!
trap 'kill $PID 2>/dev/null' EXIT
sleep 1

header "Startup"

check "health" 200 "$(code "$BASE/health")"
check "register before load" 409 "$(code -X POST "$BASE/register")"

header "CSV import (quoted CSV accepted, malformed CSV rejected, pool kept, size limit)"

printf 'A,"B, C",pw,s1,a@x\r\nA,B,"p""w",s2,\r\n' > /tmp/herd_ok.csv
check "load csv" 200 "$(code -X POST -H 'Content-Type: text/csv' --data-binary @/tmp/herd_ok.csv "$BASE/load")"
check "load bad csv" 400 "$(code -X POST -H 'Content-Type: text/csv' --data-binary 'x,y' "$BASE/load")"
check "users kept" "users_total=2" "$(curl -s "$BASE/status" | grep '^users_total')"

# oversized CSV (limit 4096 above)
head -c 5000 /dev/zero | tr '\0' 'a' > /tmp/herd_big.csv
check "oversized csv" 413 "$(code -X POST -H 'Content-Type: text/csv' --data-binary @/tmp/herd_big.csv "$BASE/load")"

header "Registration (retry key, exhaustion, reload policy)"

check "register" 201 "$(code -X POST -d request_key=k1 "$BASE/register")"
check "register retry" 200 "$(code -X POST -d request_key=k1 "$BASE/register")"
check "register 2" 201 "$(code -X POST "$BASE/register")"
check "exhausted" 409 "$(code -X POST "$BASE/register")"
check "reload after register" 409 "$(code -X POST -H 'Content-Type: text/csv' --data-binary @/tmp/herd_ok.csv "$BASE/load")"

header "Commands and status"

check "command all" 200 "$(code -X POST -d 'target=all&command=pause&pause_seconds=30' "$BASE/command")"
check "poll command" "command=pause" "$(curl -s -X POST -d 'test_id=1&state=running&ack_command_id=0' "$BASE/status" | grep '^command=')"
check "poll json" 200 "$(code -X POST -H 'Accept: application/json' -d 'test_id=1&state=paused' "$BASE/status")"
check "client has no password" 0 "$(curl -s "$BASE/client?test_id=1" | grep -c password)"
check "unknown client" 404 "$(code "$BASE/client?test_id=99")"
check "bad field" 400 "$(code -X POST -d 'test_id=1&state=running&state=idle' "$BASE/status")"

header "Protocol edges"

check "wrong method" 405 "$(code "$BASE/register")"
check "unknown path" 404 "$(code "$BASE/nope")"
check "chunked rejected" 501 "$(code -X POST -H 'Transfer-Encoding: chunked' -d 'a=1' "$BASE/register")"

header "Metrics"

METRICS="$(curl -s "$BASE/metrics")"
check "metrics clients_total" "nshtestherd_clients_total 2" "$(echo "$METRICS" | grep '^nshtestherd_clients_total')"
check "metrics no credentials" 0 "$(echo "$METRICS" | grep -c 'pw\|s1\|a@x')"

header "Shutdown"

kill $PID
wait $PID 2>/dev/null
rm -f /tmp/herd_ok.csv /tmp/herd_big.csv

header "Summary"

if [ "$FAILS" -eq 0 ]
then
  echo "[ OK ]   $CHECKS checks, $FAILS failures"
else
  echo "[ FAIL ] $CHECKS checks, $FAILS failures"
fi

echo ""

[ "$FAILS" -eq 0 ]
