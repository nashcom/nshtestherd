#!/bin/bash
#
# run.sh - hands-free, end-to-end run of the k6 worker example
#
# Needs only Docker and curl. It starts the coordinator and the k6 workers in containers, drives them through
# run -> pause -> resume -> stop with the nshtestherd API, checks every step, prints the k6 summary and cleans up.
# Nothing on your machine is changed except Docker containers/networks named nshtestherd-k6-demo-* (removed at the end)
# and, on first use, the Docker images (nshtestherd:latest is built from this repository).
#
# Usage: examples/k6/run.sh [--keep] [--help]
#
# Settings (environment):
#   VUS            number of k6 workers                       (default 5)
#   USERS          accounts generated for the coordinator     (default VUS + 5)
#   PORT           coordinator port on 127.0.0.1              (default 8788)
#   POLL_SECONDS   worker polling interval                    (default 1)
#   RUN_SECONDS    how long the workers stay in "run"         (default 10)
#   PAUSE_SECONDS  length of the timed pause                  (default 6)
#   HERD_IMAGE     use this nshtestherd image instead of building one,
#                  for example ghcr.io/nashcom/nshtestherd:latest
#   K6_IMAGE       k6 image                                   (default grafana/k6:latest)
#   KEEP=1         (or --keep) leave the coordinator running at the end

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
SCRIPT="$HERE/herd-worker.js"

VUS="${VUS:-5}"
USERS="${USERS:-$((VUS + 5))}"
PORT="${PORT:-8788}"
POLL_SECONDS="${POLL_SECONDS:-1}"
RUN_SECONDS="${RUN_SECONDS:-10}"
PAUSE_SECONDS="${PAUSE_SECONDS:-6}"
HERD_IMAGE="${HERD_IMAGE:-}"
K6_IMAGE="${K6_IMAGE:-grafana/k6:latest}"
KEEP="${KEEP:-0}"

NAME="nshtestherd-k6-demo"
NET="$NAME"
HERD_C="$NAME-herd"
K6_C="$NAME-k6"
H="http://127.0.0.1:$PORT"
LINE="$(printf '%90s' '' | tr ' ' '-')"
WORK="$(mktemp -d)"
K6_PID=""
CHECKS=0
FAILS=0


usage()
{
  # the comment block at the top of this file, without the leading "# "
  awk 'NR >= 3 && /^#/ { sub(/^# ?/, ""); print; next } NR >= 3 { exit }' "$0"
}


header()
{
  echo ""
  echo "$LINE"
  echo "$1"
  echo "$LINE"
  echo ""
}


pass()
{
  CHECKS=$((CHECKS + 1))
  echo "[ OK ]   $1"
}


fail()
{
  CHECKS=$((CHECKS + 1))
  FAILS=$((FAILS + 1))
  echo "[ FAIL ] $1"
}


cleanup()
{
  if [ -n "$K6_PID" ]
  then
    docker rm -f "$K6_C" >/dev/null 2>&1
  fi

  if [ "$KEEP" = "1" ]
  then
    return
  fi

  docker rm -f "$HERD_C" >/dev/null 2>&1
  docker network rm "$NET" >/dev/null 2>&1
  rm -rf "$WORK"
}


# Prints the summary and leaves with the right exit code
finish()
{
  header "Summary"

  if [ "$FAILS" -eq 0 ]
  then
    echo "[ OK ]   $CHECKS checks, $FAILS failures"
  else
    echo "[ FAIL ] $CHECKS checks, $FAILS failures"

    if [ -f "$WORK/k6.log" ]
    then
      echo ""
      echo "Last lines of the k6 output:"
      tail -n 15 "$WORK/k6.log"
    fi

    if docker ps -a --format '{{.Names}}' 2>/dev/null | grep -qx "$HERD_C"
    then
      echo ""
      echo "Last lines of the coordinator log:"
      docker logs --tail 15 "$HERD_C" 2>&1
    fi
  fi

  if [ "$KEEP" = "1" ]
  then
    echo ""
    echo "Coordinator left running on $H (docker rm -f $HERD_C && docker network rm $NET to remove it)"
  fi

  echo ""
  [ "$FAILS" -eq 0 ]
  exit $?
}


die()
{
  fail "$1"
  finish
}


# One value from GET /status, empty if the coordinator does not answer
status_value()
{
  curl -s -m 5 "$H/status" | sed -n "s/^$1=//p" | tr -d '\r'
}


# wait_until <description> <status key> <expected value> <timeout seconds>
wait_until()
{
  local desc="$1" key="$2" want="$3" limit="$4" waited=0 got=""

  while [ "$waited" -lt "$limit" ]
  do
    got="$(status_value "$key")"

    if [ "$got" = "$want" ]
    then
      pass "$desc ($key=$got)"
      return 0
    fi

    sleep 1
    waited=$((waited + 1))
  done

  fail "$desc ($key=$got, expected $want after ${limit}s)"
  return 1
}


# Sends a command to all clients and checks that every worker was reached
herd_command()
{
  local body="$1" reply

  reply="$(curl -s -m 10 -X POST -d "target=all&$body" "$H/command" | tr -d '\r')"

  if echo "$reply" | grep -qx "updated=$VUS"
  then
    pass "command '$body' sent to all $VUS workers"
    return 0
  fi

  fail "command '$body' did not reach $VUS workers (reply: $(echo "$reply" | tr '\n' ' '))"
  return 1
}


# Number of GET /health requests the coordinator has logged (the workers' target requests)
target_hits()
{
  docker logs "$HERD_C" 2>&1 | grep -c 'GET /health'
}


show_status()
{
  echo "         $(curl -s -m 5 "$H/status" | grep '^clients_' | tr '\n' ' ')"
}


prerequisites()
{
  header "Prerequisites"

  command -v docker >/dev/null 2>&1 || die "docker not found"
  docker info >/dev/null 2>&1 || die "the Docker daemon is not reachable"
  command -v curl >/dev/null 2>&1 || die "curl not found"
  [ -f "$SCRIPT" ] || die "$SCRIPT not found"

  if (exec 3<>"/dev/tcp/127.0.0.1/$PORT") 2>/dev/null
  then
    die "port $PORT is in use - set PORT to a free port"
  fi

  pass "docker, curl and the k6 script are available, port $PORT is free"
}


prepare_images()
{
  header "Images"

  if [ -n "$HERD_IMAGE" ]
  then
    IMAGE="$HERD_IMAGE"
    echo "Pulling $IMAGE ..."
    docker pull "$IMAGE" >"$WORK/pull.log" 2>&1 || die "cannot pull $IMAGE"
  else
    IMAGE="nshtestherd:latest"
    echo "Building $IMAGE from $ROOT (a minute on the first run) ..."
    docker build -t "$IMAGE" "$ROOT" >"$WORK/build.log" 2>&1 || { tail -n 20 "$WORK/build.log"; die "docker build failed"; }
  fi

  pass "coordinator image: $IMAGE"

  echo "Pulling $K6_IMAGE ..."
  docker pull "$K6_IMAGE" >"$WORK/pull-k6.log" 2>&1 || die "cannot pull $K6_IMAGE"
  pass "k6 image: $K6_IMAGE"
}


start_coordinator()
{
  header "Start the coordinator ($USERS generated accounts)"

  # leftovers of an earlier, interrupted run
  docker rm -f "$HERD_C" "$K6_C" >/dev/null 2>&1
  docker network rm "$NET" >/dev/null 2>&1

  docker network create "$NET" >/dev/null || die "cannot create the Docker network"

  docker run -d --name "$HERD_C" --network "$NET" --network-alias herd -p "127.0.0.1:$PORT:8788" \
    "$IMAGE" --bind 0.0.0.0 --generate "$USERS" --verbose >/dev/null || die "cannot start the coordinator"

  local waited=0

  while [ "$waited" -lt 30 ]
  do
    if curl -fs -m 2 "$H/health" >/dev/null 2>&1
    then
      pass "coordinator answers on $H/health"
      return 0
    fi

    sleep 1
    waited=$((waited + 1))
  done

  die "the coordinator did not answer on $H/health"
}


start_workers()
{
  header "Start $VUS k6 workers"

  # The script comes in on standard input, so nothing has to be mounted into the container.
  # The target of the workers is /health of the coordinator: easy to count in its log.
  docker run --rm -i --name "$K6_C" --network "$NET" \
    -e HERD_URL=http://herd:8788 -e TARGET_URL=http://herd:8788/health \
    -e VUS="$VUS" -e DURATION=10m -e POLL_SECONDS="$POLL_SECONDS" \
    "$K6_IMAGE" run - < "$SCRIPT" > "$WORK/k6.log" 2>&1 &
  K6_PID=$!

  wait_until "all workers registered" clients_total "$VUS" 60 || finish
  show_status
}


drive_the_test()
{
  local before after hold during_before during_after resumed_before resumed_after

  header "Run: workers start requesting the target"

  before="$(target_hits)"
  herd_command "command=run&job=demo" || finish
  wait_until "all workers running" clients_running "$VUS" 30 || finish

  echo "         letting the workers run for $RUN_SECONDS seconds ..."
  sleep "$RUN_SECONDS"
  show_status

  after="$(target_hits)"

  if [ "$((after - before))" -ge "$VUS" ]
  then
    pass "workers requested the target: $((after - before)) requests in the coordinator log"
  else
    fail "workers did not request the target (only $((after - before)) requests logged)"
  fi

  header "Pause for $PAUSE_SECONDS seconds, then resume"

  herd_command "command=pause&pause_seconds=$PAUSE_SECONDS" || finish
  wait_until "all workers paused" clients_paused "$VUS" 30 || finish
  show_status

  # The reported state is not enough: the workload itself must really be suspended. A worker that
  # reports "paused" makes no target request, so the request count must stand still during the pause.
  hold=$((PAUSE_SECONDS - 4))

  if [ "$hold" -gt 2 ]
  then
    hold=2
  fi

  if [ "$hold" -ge 1 ]
  then
    during_before="$(target_hits)"
    sleep "$hold"
    during_after="$(target_hits)"

    if [ "$during_after" -eq "$during_before" ]
    then
      pass "no target requests while paused (0 requests in ${hold}s of pause)"
    else
      fail "workers kept requesting the target while paused ($((during_after - during_before)) requests in ${hold}s)"
    fi
  else
    echo "         PAUSE_SECONDS=$PAUSE_SECONDS is too short to verify that the workload stands still (use 5 or more)"
  fi

  wait_until "all workers resumed" clients_running "$VUS" $((PAUSE_SECONDS + 30)) || finish

  resumed_before="$(target_hits)"
  sleep 3
  resumed_after="$(target_hits)"

  if [ "$((resumed_after - resumed_before))" -ge "$VUS" ]
  then
    pass "workers requested the target again after the resume ($((resumed_after - resumed_before)) requests in 3s)"
  else
    fail "workers did not request the target again after the resume ($((resumed_after - resumed_before)) requests in 3s)"
  fi

  header "Stop"

  herd_command "command=stop" || finish
  wait_until "all workers done" clients_done "$VUS" 30 || finish
  show_status
}


check_metrics()
{
  local reg upd

  header "Coordinator metrics"

  curl -s -m 5 "$H/metrics" | grep -E '^nshtestherd_(users_|clients_by_state|registrations_total|command_updates_total|status_reports_total)' | sed 's/^/         /'
  echo ""

  reg="$(curl -s -m 5 "$H/metrics" | sed -n 's/^nshtestherd_registrations_total //p' | tr -d '\r')"
  upd="$(curl -s -m 5 "$H/metrics" | sed -n 's/^nshtestherd_command_updates_total //p' | tr -d '\r')"

  if [ "$reg" = "$VUS" ]
  then
    pass "registrations_total=$reg (one per worker)"
  else
    fail "registrations_total=$reg, expected $VUS"
  fi

  if [ "$upd" = "$((VUS * 3))" ]
  then
    pass "command_updates_total=$upd (run, pause and stop for every worker)"
  else
    fail "command_updates_total=$upd, expected $((VUS * 3))"
  fi
}


k6_summary()
{
  header "k6 summary"

  # SIGTERM makes k6 stop the test and print its end-of-test summary
  docker stop -t 20 "$K6_C" >/dev/null 2>&1
  wait "$K6_PID" 2>/dev/null
  K6_PID=""

  # k6 prints a progress line every second ("running (00m07.0s), ..."): leave those out
  grep -v '^running (' "$WORK/k6.log" | grep -E 'registered as|checks|http_reqs|http_req_failed|iterations|herd_|target' | tail -n 20

  echo ""

  if grep -E 'checks.*: 100\.00%' "$WORK/k6.log" >/dev/null 2>&1
  then
    pass "k6: every check passed (target answers 200)"
  else
    fail "k6: the checks did not all pass, or no summary was printed"
  fi
}


for arg in "$@"
do
  case "$arg" in
    --keep)
      KEEP=1
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $arg"
      usage
      exit 2
      ;;
  esac
done

trap cleanup EXIT

prerequisites
prepare_images
start_coordinator
start_workers
drive_the_test
check_metrics
k6_summary
finish
