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
#   - every job that ends (unknown job: failed, replaced by a newer idle/run/stop:
#     stopped) is reported with last_job_id/last_job/last_job_result, and the
#     worker is idle again: a job never ends the worker
#   - stop: report stopping, then done, and exit
#
# Usage: ./worker.sh [base-url]      (default http://127.0.0.1:8788)
#        NSHTEST_TOKEN=...           the coordinator's token, when it has one

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
MESSAGE="waiting for work"
JOB=""                # the current job (also while paused), empty: none
JOB_ID=0              # command_id of the run command that started it
APPLIED_ID=0          # last command_id applied (sent back as ack_command_id)
PAUSE_UNTIL=0         # $SECONDS value at which a timed pause ends
RESUME_STATE="idle"   # state to return to after a pause

# The last job that ended, sent with every report (the coordinator counts it once)
LAST_JOB_ID=0
LAST_JOB=""
LAST_JOB_RESULT=""


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
  local auth=()
  shift

  # The coordinator's token, when it has one
  if [ -n "$NSHTEST_TOKEN" ]
  then
    auth=(-H "Authorization: Bearer $NSHTEST_TOKEN")
  fi

  curl -sS -m 10 -X POST "${auth[@]}" "$@" "$BASE$path"
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
  local last_job=()

  if [ "$LAST_JOB_ID" -gt 0 ]
  then
    last_job=(--data-urlencode "last_job_id=$LAST_JOB_ID" --data-urlencode "last_job=$LAST_JOB" --data-urlencode "last_job_result=$LAST_JOB_RESULT")
  fi

  reply=$(post /status \
    --data-urlencode "test_id=$TEST_ID" \
    --data-urlencode "state=$STATE" \
    --data-urlencode "ack_command_id=$APPLIED_ID" \
    --data-urlencode "message=$message" \
    "${last_job[@]}")

  parse_response "$reply"

  if [ -z "${R[command_id]}" ]
  then
    log "status report failed: ${R[message]:-no response}"
    return 1
  fi

  return 0
}


# The current job ended: remember the result for the reports, back to idle.
# $1 result (ok, failed, stopped), $2 message
end_job()
{
  LAST_JOB_ID="$JOB_ID"
  LAST_JOB="$JOB"
  LAST_JOB_RESULT="$1"
  JOB=""
  STATE="idle"
  RESUME_STATE="idle"
  MESSAGE="$2"
  log "job $LAST_JOB $1: $2"
}


# Applies a new instruction. Called once per command_id.
apply_instruction()
{
  # report() overwrites R: keep what this command says
  local id="${R[command_id]}"
  local command="${R[command]}"
  local job="${R[job]}"
  local pause="${R[pause_seconds]}"

  # A new idle, run or stop ends the job that is still there (a pause keeps it).
  # Reported on its own, so the result keeps its own message.
  if [ "$command" != "pause" ] && [ -n "$JOB" ]
  then
    end_job stopped "job $JOB stopped"
    report "$MESSAGE"
  fi

  APPLIED_ID="$id"

  case "$command" in
    idle)
      STATE="idle"
      MESSAGE="waiting for work"
      ;;
    run)
      JOB_ID="$id"
      JOB="$job"

      case "$KNOWN_JOBS" in
        *" $job "*)
          STATE="running"
          MESSAGE="job $job"
          ;;
        *)
          # An unknown job fails; the worker stays and waits for the next command
          end_job failed "unknown job: $job"
          ;;
      esac
      ;;
    pause)
      # Remember what to resume, unless we are already paused
      if [ "$STATE" != "paused" ]
      then
        RESUME_STATE="$STATE"
      fi

      STATE="paused"
      PAUSE_UNTIL=$(( SECONDS + pause ))
      MESSAGE="paused ${pause}s"
      ;;
    stop)
      STATE="stopping"
      report "stopping"
      log "stopping"
      STATE="done"
      report "finished"
      log "done"
      exit 0
      ;;
  esac

  log "applied command $id: $command $job"
}


do_one_operation()
{
  # Placeholder for one unit of load work; safe point for switching jobs
  log "running ${JOB}"
}


register

while true
do
  if report "$MESSAGE"
  then
    # Apply each command_id once; a repeated poll with the same id changes nothing
    if [ "${R[command_id]}" -gt "$APPLIED_ID" ]
    then
      apply_instruction
      continue   # report the new state and acknowledgement right away
    fi
  fi

  # Timed pause ends on the worker's own monotonic clock
  if [ "$STATE" = "paused" ] && [ "$SECONDS" -ge "$PAUSE_UNTIL" ]
  then
    STATE="$RESUME_STATE"
    MESSAGE="pause over"
    log "pause over, back to $STATE"
  fi

  if [ "$STATE" = "running" ]
  then
    do_one_operation
  fi

  sleep "$POLL_SECONDS"
done
