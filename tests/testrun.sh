#!/bin/bash
#
# testrun.sh - longer run of a real coordinator with two runners, checks that the job results add up
#
# Usage: tests/testrun.sh [seconds] [path-to-nshtestherd]
#   seconds   how long random commands are sent (default 60)
# Environment: HERD_TEST_PORT (default 18789). Refuses to run if the port is in use.
#
# Two runners with 5 clients each: one runs a small bash job (sleeps 0-3 s, fails about one run in four), the other the
# built-in dummy job (never ends by itself, so it ends as "stopped"). Random run, idle and pause commands go to all
# clients or to one. Then everything is stopped and the coordinator's counts are compared with the runner logs: every
# job a client logged as ok, failed or stopped must be counted exactly once, and /metrics must say the same as /status.

DURATION="${1:-60}"
BIN="${2:-./nshtestherd}"
PORT="${HERD_TEST_PORT:-18789}"

# The checks expect a coordinator without a token, whatever the calling shell has set
unset NSHTEST_TOKEN
BASE="http://127.0.0.1:$PORT"
CLIENTS_PER_RUNNER=5
CLIENTS=$((CLIENTS_PER_RUNNER * 2))
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


# A field of GET /status, for example: status_field jobs_ok
status_field()
{
  curl -s "$BASE/status" | grep "^$1=" | cut -d= -f2
}


# A job result counted in /metrics
metric_jobs()
{
  echo "$METRICS" | grep "^nshtestherd_jobs_ended_total{result=\"$1\"}" | cut -d' ' -f2
}


# Job results logged by the runners: "[client 3 id 7] job j12 ok: ..." (job names have no blanks)
logged_jobs()
{
  cat "$WORK/runner_program.log" "$WORK/runner_dummy.log" | grep -cE "\] job [^ ]+ $1(:|\$)"
}


send_command()
{
  curl -s -o /dev/null -w '%{http_code}' -X POST -d "$1" "$BASE/command"
}


if ! [[ "$DURATION" =~ ^[0-9]+$ ]] || [ "$DURATION" -lt 10 ]
then
  echo "Usage: tests/testrun.sh [seconds, at least 10] [path-to-nshtestherd]"
  exit 2
fi

if (exec 3<>"/dev/tcp/127.0.0.1/$PORT") 2>/dev/null
then
  echo "Port $PORT is in use - set HERD_TEST_PORT to a free port"
  exit 2
fi

WORK="$(mktemp -d)"

header "Start coordinator on port $PORT and two runners ($CLIENTS clients), logs in $WORK"

"$BIN" --port "$PORT" --generate 20 --timeout 3 > "$WORK/server.log" 2>&1 &
SERVER_PID=$!
trap 'kill $SERVER_PID $PROGRAM_PID $DUMMY_PID 2>/dev/null' EXIT
sleep 1

check "health" "ok" "$(curl -s "$BASE/health" | grep '^status=' | cut -d= -f2)"

"$BIN" --runner --server "$BASE" --clients "$CLIENTS_PER_RUNNER" --poll-seconds 1 \
  --program /bin/bash -- -c 'sleep $((RANDOM % 4)); [ $((RANDOM % 4)) -ne 0 ]' > "$WORK/runner_program.log" 2>&1 &
PROGRAM_PID=$!

"$BIN" --runner --server "$BASE" --clients "$CLIENTS_PER_RUNNER" --poll-seconds 1 > "$WORK/runner_dummy.log" 2>&1 &
DUMMY_PID=$!

for WAIT in $(seq 1 15)
do
  [ "$(status_field clients_idle)" = "$CLIENTS" ] && break
  sleep 1
done

check "all clients registered and idle" "$CLIENTS" "$(status_field clients_idle)"

header "Random commands for $DURATION seconds"

END=$((SECONDS + DURATION))
SENT=0
REJECTED=0
JOB=0

while [ "$SECONDS" -lt "$END" ]
do
  PICK=$((RANDOM % 100))
  JOB=$((JOB + 1))

  if [ "$PICK" -lt 45 ]
  then
    CODE="$(send_command "target=all&command=run&job=j$JOB")"
  elif [ "$PICK" -lt 65 ]
  then
    CODE="$(send_command "test_id=$((RANDOM % CLIENTS + 1))&command=run&job=j$JOB")"
  elif [ "$PICK" -lt 80 ]
  then
    CODE="$(send_command "target=all&command=idle")"
  elif [ "$PICK" -lt 90 ]
  then
    CODE="$(send_command "test_id=$((RANDOM % CLIENTS + 1))&command=idle")"
  else
    CODE="$(send_command "target=all&command=pause&pause_seconds=$((RANDOM % 3 + 1))")"
  fi

  SENT=$((SENT + 1))
  [ "$CODE" = "200" ] || REJECTED=$((REJECTED + 1))

  sleep $((RANDOM % 3 + 1))
done

echo "$SENT commands sent"
check "commands accepted" 0 "$REJECTED"

header "Stop all clients"

check "stop all" 200 "$(send_command "target=all&command=stop")"

# A running bash job ends first (at most 3 s), then the runner sees the stop
for WAIT in $(seq 1 30)
do
  kill -0 $PROGRAM_PID 2>/dev/null || kill -0 $DUMMY_PID 2>/dev/null || break
  sleep 1
done

check "program runner ended" "ended" "$(kill -0 $PROGRAM_PID 2>/dev/null && echo running || echo ended)"
check "dummy runner ended" "ended" "$(kill -0 $DUMMY_PID 2>/dev/null && echo running || echo ended)"
check "all clients done" "$CLIENTS" "$(status_field clients_done)"
check "no client running" 0 "$(status_field clients_running)"
check "no client in error" 0 "$(status_field clients_error)"

header "Job results: coordinator against the runner logs"

METRICS="$(curl -s "$BASE/metrics")"
TOTAL=0

for RESULT in ok failed stopped
do
  LOGGED="$(logged_jobs "$RESULT")"
  COUNTED="$(status_field "jobs_$RESULT")"
  TOTAL=$((TOTAL + LOGGED))

  echo "         $RESULT: runners logged $LOGGED, coordinator counted $COUNTED"
  check "jobs $RESULT: /status = runner logs" "$LOGGED" "$COUNTED"
  check "jobs $RESULT: /metrics = /status" "$COUNTED" "$(metric_jobs "$RESULT")"
done

check "jobs ran at all" "yes" "$([ "$TOTAL" -gt 0 ] && echo yes || echo no)"

header "Shutdown"

kill $SERVER_PID
wait $SERVER_PID 2>/dev/null

header "Summary"

if [ "$FAILS" -eq 0 ]
then
  echo "[ OK ]   $CHECKS checks, $FAILS failures ($TOTAL jobs in $DURATION seconds)"
  rm -rf "$WORK"
else
  echo "[ FAIL ] $CHECKS checks, $FAILS failures - logs kept in $WORK"
fi

echo ""

[ "$FAILS" -eq 0 ]
