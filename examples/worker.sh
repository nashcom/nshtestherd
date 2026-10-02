#!/bin/bash
#
# worker.sh - example nshtestherd worker (no Domino calls)
#
# Shows the worker side of the protocol:
#   - register once with a generated request_key (safe to retry)
#   - poll /status, report observed state and the last applied command_id
#   - apply each command_id exactly once (repeated polls do not restart a job
#     or reset a pause timer)
#   - timed pause on a monotonic timer ($SECONDS), then resume the previous state
#   - stop: report stopping, then done, and exit
#
# Usage: ./worker.sh [base-url]      (default http://127.0.0.1:8788)

BASE="${1:-${HERD_URL:-http://127.0.0.1:8788}}"
POLL_SECONDS="${POLL_SECONDS:-2}"
KNOWN_JOBS=" mail-read mail-write idle-demo "

# Opaque key, generated once, reused for every registration retry
REQUEST_KEY="$(cat /proc/sys/kernel/random/uuid 2>/dev/null)"

if [ -z "$REQUEST_KEY" ]
then
  REQUEST_KEY="worker-$$-$RANDOM$RANDOM"
fi

declare -A R

TEST_ID=""
STATE="idle"          # observed state
JOB=""
APPLIED_ID=0          # last command_id applied (sent back as ack_command_id)
PAUSE_UNTIL=0         # $SECONDS value at which a timed pause ends
RESUME_STATE="idle"   # state to return to after a pause


log()
{
  echo "[worker ${TEST_ID:-?}] $*"
}


# Splits "key=value" lines into the R array. Values may contain '=' and spaces.
# This simple worker does not unescape \n \t \\ (none of its fields contain them).
parse_response()
{
  R=()
  local line

  while IFS= read -r line
  do
    if [ -n "$line" ]
    then
      R["${line%%=*}"]="${line#*=}"
    fi
  done <<< "$1"
}


post()
{
  local path="$1"
  shift
  curl -sS -m 10 -X POST "$@" "$BASE$path"
}


register()
{
  local reply

  until [ -n "$TEST_ID" ]
  do
    reply=$(post /register --data-urlencode "request_key=$REQUEST_KEY")
    parse_response "$reply"

    if [ -n "${R[test_id]}" ]
    then
      TEST_ID="${R[test_id]}"
      log "registered as ${R[shortname]} (command ${R[command]}, id ${R[command_id]})"
      # A real worker would log in with ${R[password]} here - never print it.
    else
      log "registration failed: ${R[message]:-no response}; retrying with the same key"
      sleep "$POLL_SECONDS"
    fi
  done
}


# Reports state, acknowledges the last applied command, parses the reply into R.
report()
{
  local message="$1"
  local reply

  reply=$(post /status \
    --data-urlencode "test_id=$TEST_ID" \
    --data-urlencode "state=$STATE" \
    --data-urlencode "ack_command_id=$APPLIED_ID" \
    --data-urlencode "message=$message")

  parse_response "$reply"

  if [ -z "${R[command_id]}" ]
  then
    log "status report failed: ${R[message]:-no response}"
    return 1
  fi

  return 0
}


fail()
{
  STATE="error"
  report "$1"
  log "error: $1"
  exit 1
}


# Applies a new instruction. Called once per command_id.
apply_instruction()
{
  local id="${R[command_id]}"

  case "${R[command]}" in
    idle)
      STATE="idle"
      JOB=""
      ;;
    run)
      case "$KNOWN_JOBS" in
        *" ${R[job]} "*)
          ;;
        *)
          APPLIED_ID="$id"
          fail "unknown job: ${R[job]}"
          ;;
      esac

      JOB="${R[job]}"
      STATE="running"
      ;;
    pause)
      # Remember what to resume, unless we are already paused
      if [ "$STATE" != "paused" ]
      then
        RESUME_STATE="$STATE"
      fi

      STATE="paused"
      PAUSE_UNTIL=$(( SECONDS + R[pause_seconds] ))
      ;;
    stop)
      STATE="stopping"
      APPLIED_ID="$id"
      report "stopping"
      log "stopping"
      STATE="done"
      report "finished"
      log "done"
      exit 0
      ;;
  esac

  APPLIED_ID="$id"
  log "applied command $id: ${R[command]} ${R[job]} ${R[pause_seconds]/#0/}"
}


do_one_operation()
{
  # Placeholder for one unit of load work; safe point for switching jobs
  log "running ${JOB}"
}


register

while true
do
  if report ""
  then
    # Apply each command_id once; a repeated poll with the same id changes nothing
    if [ "${R[command_id]}" -gt "$APPLIED_ID" ]
    then
      apply_instruction
    fi
  fi

  # Timed pause ends on the worker's own monotonic clock
  if [ "$STATE" = "paused" ] && [ "$SECONDS" -ge "$PAUSE_UNTIL" ]
  then
    STATE="$RESUME_STATE"
    log "pause over, back to $STATE"
  fi

  if [ "$STATE" = "running" ]
  then
    do_one_operation
  fi

  sleep "$POLL_SECONDS"
done
