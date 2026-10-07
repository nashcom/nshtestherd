# Writing a program for the runner

The runner (`nshtestherd --runner --program <exe> -- <args>`) starts your program once per client and `run` command.
This page is the contract between the runner and that program. It applies to any language: a test program, a Bash script,
a Notes client tool.

## How the program is started

```
coordinator: run command          runner (one logical client)                your program (child)
------------------------          ---------------------------                --------------------
POST /command run job=NAME  -->   next poll sees command_id N
                                  applies it: starts the program     -->     starts, reads NSH_* variables
                                  reports state=running, ack=N              does its work
                                  ...                                        exits
                                  child finished: exit 0 -> idle     <--     exit code 0 = OK, else = failure
                                                exit != 0 -> error (final)
```

- **One process per client.** `--clients 50` means up to 50 children at the same time, each with its own account. That suits
  tools that can only act as one identity per process (for example a Notes client).
- **No shell.** The program is started directly (no `sh -c`, no `cmd.exe`): it must be a real executable. Arguments are passed
  as separate arguments, so spaces in values do not split them.
- **One run per `run` command.** A `run` command with a new `command_id` starts a new child. A child that has finished is
  **not** started again for an `command_id` that was already acknowledged.
- **Inherited:** the working directory, standard input and output of the runner. The output of your program appears in the
  runner's console as it is (not prefixed, not captured): put the client number in your own log lines.

## What your program receives

Environment variables, always:

| Variable                                                                | Value                              |
| ----------------------------------------------------------------------- | ---------------------------------- |
| `NSH_TEST_ID`                                                           | sequential client number           |
| `NSH_FIRSTNAME`, `NSH_LASTNAME`, `NSH_SHORTNAME`, `NSH_INTERNETADDRESS` | the allocated account              |
| `NSH_PASSWORD`                                                          | the allocated password             |
| `NSH_JOB`                                                               | job name from the `run` command    |
| `NSH_COMMAND_ID`                                                        | command id that started this child |
| `NSH_SERVER`                                                            | coordinator URL                    |

Arguments: everything after `--` on the runner's command line, the same for every child, with placeholders replaced per
client. A placeholder is an environment variable name in braces, for example:

```bash
nshtestherd --runner --clients 50 --program mytests -- --user {NSH_SHORTNAME} --id-file /tmp/ids/{NSH_TEST_ID}.id --scenario {NSH_JOB}
```

- `{NSH_PASSWORD}` is refused: arguments are visible to every user in the process list. Read the password from the
  environment variable.
- An unknown `{NSH_...}` name is refused at startup (exit code 2). Other text with braces is literal.
- Do not use `$NSH_SHORTNAME` or `${NSH_SHORTNAME}` in the runner's command line: your shell or Docker Compose expands
  that before the runner sees it.

## What your program returns

| Exit code          | The runner reports                              | Then                            |
| ------------------ | ----------------------------------------------- | ------------------------------- |
| `0`                | `idle`, "job NAME finished (exit 0)"            | Waits for the next command      |
| non-zero           | `error`, "job NAME failed (exit N)" (**final**) | Client finished; runner exits 1 |
| killed by a signal | `error`, exit code `128 + signal` (Linux)       | As above                        |
| cannot be started  | `error`, with the reason (also printed)         | As above                        |

The runner only sees the exit code. Print the reason for a failure on standard error (it appears in the runner's
console) and use a non-zero exit code only for real failures.

## Pause and stop

The runner applies `pause`, a new `run` and `stop` **between** two child executions. While your program runs, newer
commands wait; the runner acknowledges them when it applies them. So a program that runs for a long time ignores `pause`
and `stop` unless it cooperates:

- **Read the desired command, read-only.** `GET $NSH_SERVER/client?test_id=$NSH_TEST_ID` returns the fields `command`,
  `command_id`, `job` and `pause_seconds` (and the state; never the password).
- **Never report status yourself.** The runner is the only one that sends `POST /status` for this client. A second
  reporter under the same `test_id` would overwrite the runner's state and acknowledgements.
- **Stop:** when `command=stop`, finish the current operation, clean up and exit with `0`. The runner then applies the
  pending `stop` and reports `done`.
- **Pause:** exit with `0` when `command=pause` (and your current operation is finished). The runner applies the pause and
  starts its timer. With an external program a pause therefore ends the current run: it resumes as `idle`, and work
  continues with the next `run` command. (A program that wants to pause in place, without exiting, can wait
  `pause_seconds` itself and carry on; the runner then acknowledges the command only after the program exits.)
- **Ctrl+C or `docker stop` on the runner** stops the children: SIGTERM first, SIGKILL after 5 seconds on Linux
  (immediate termination on Windows). Handle SIGTERM and clean up, for example delete downloaded ID files.

A well-behaved long-running program in Bash:

```bash
#!/bin/bash
# example: a program for the runner that stops cooperatively

desired_command()
{
  curl -s -m 5 "$NSH_SERVER/client?test_id=$NSH_TEST_ID" | sed -n 's/^command=//p' | tr -d '\r'
}

cleanup()
{
  rm -f "/tmp/ids/$NSH_TEST_ID.id"
}

trap cleanup EXIT
trap 'exit 0' TERM

echo "client $NSH_TEST_ID ($NSH_SHORTNAME) starting job $NSH_JOB"

while true
do
  if [ "$(desired_command)" = "stop" ]
  then
    echo "client $NSH_TEST_ID: stop requested, finishing"
    exit 0
  fi

  # one unit of work goes here
  sleep 1
done
```

## Do and do not

- **Do** read the password from `NSH_PASSWORD` and never write it to a log, an argument or a file that outlives the run.
- **Do** clean up temporary files (ID files, caches) on exit and on SIGTERM.
- **Do** exit non-zero for a failed test run and zero for a clean finish, including a requested stop.
- **Do not** call `POST /status`, `POST /command` or `POST /load` from the program.
- **Do not** assume other clients exist: each child sees only its own account.

## Try a program without the real system

1. Start a coordinator with generated accounts: `./nshtestherd --generate 10`.
2. Start the runner with your program: `./nshtestherd --runner --clients 2 --program ./myprogram -- --user {NSH_SHORTNAME}`.
3. Send the command: `curl -X POST -d 'target=all&command=run&job=demo' http://127.0.0.1:8788/command`.
4. Watch `curl http://127.0.0.1:8788/status` and the runner's console. A finished child shows as `idle` with
   `finished (exit 0)`; a failing one as `error`.

To see what a child receives, use the binary itself as the program (`--program ./nshtestherd -- --child-info`): it prints
its process id and the `NSH_*` values (never the password). The container route works the same way, see the README.

## Checklist for a new program

- [ ] Reads its identity from `NSH_SHORTNAME` / `NSH_PASSWORD` (and the other `NSH_*` variables it needs).
- [ ] Uses `{NSH_...}` placeholders for per-client arguments, never `{NSH_PASSWORD}`.
- [ ] Exits `0` on success and on a requested stop, non-zero on failure, with the reason on standard error.
- [ ] Handles SIGTERM and cleans up its files.
- [ ] If it runs long: checks `GET /client` for `stop` and exits cleanly.
- [ ] Does not report status itself.
