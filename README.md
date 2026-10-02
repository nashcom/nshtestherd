# nshtestherd

Small standalone coordinator for load/test workers. It imports existing test
accounts, hands each registering worker one account plus a sequential test
client number, receives worker status, delivers commands, and exposes
Prometheus metrics.

It does **not** provision users and does **not** run load itself (the optional
[runner](#runner-optional) can launch your load program). It has no Domino
dependency. Any HTTP client (C/C++, LotusScript, Bash, k6, curl) can be a worker.

Plain C++17 + OS sockets, no external libraries. Linux and Windows. All state
is in memory; a restart begins a fresh herd (client numbering starts at 1, all
accounts are free again). Stop the previous workers before restarting.
Trusted test network assumed: no authentication, no TLS.

One executable, two modes:

```
                 HTTP (public API, see docs/PROTOCOL.md)
  nshtestherd  <--------------------------------------------  nshtestherd --runner
  (server, default)      register / status / command           (optional worker host)
   - account pool                                               - N logical clients
   - clients, commands                                          - dummy job, or one child
   - /metrics                                                     process per client
        ^
        |  any other HTTP client works the same way: Bash, k6, LotusScript, C/C++, curl
```

## Status

| Area                                     | State                                                                 |
| ---------------------------------------- | --------------------------------------------------------------------- |
| Server, runner, CSV import, `--generate` | Complete                                                              |
| Tested on                                | Linux (g++, GNU make): all test suites pass                           |
| Windows build                            | Written (Winsock, `CreateProcess`), **not yet built or tested**       |
| Not tested                               | Heavy load (503 on queue overflow, slow clients, 1000 runner clients) |

## Quick start

```bash
make
./nshtestherd --csv examples/users.csv     # or without a CSV: ./nshtestherd --generate 100
```

In another shell:

```bash
curl -X POST -d request_key=my-worker-1 http://127.0.0.1:8788/register
curl -X POST -d 'target=all&command=run&job=mail-read' http://127.0.0.1:8788/command
curl http://127.0.0.1:8788/metrics
./examples/worker.sh        # example Bash worker
```

Server and runner in two windows (no external program needed):

```bash
# window 1
./nshtestherd --generate 20 --verbose
# window 2
./nshtestherd --runner --clients 5 --program ./nshtestherd -- --child-info
# window 3
curl -X POST -d 'target=all&command=run&job=demo' http://127.0.0.1:8788/command
```

Full API with curl examples: [docs/PROTOCOL.md](docs/PROTOCOL.md).

## Concepts

| Term           | Meaning                                                                                                     |
| -------------- | ----------------------------------------------------------------------------------------------------------- |
| Account pool   | The imported (or generated) users, in order. Each account is handed out once.                               |
| Client         | One registered worker. Gets the next free account and the next `test_id` (1, 2, 3, ...).                    |
| `test_id`      | Sequential client number assigned by the server. Never reused during one run, independent of account names. |
| `request_key`  | Optional opaque key a worker generates once and repeats on retries, so a retry returns the same allocation. |
| Command        | What the coordinator wants a client to do: `idle`, `run <job>`, `pause <seconds>`, `stop`.                  |
| `command_id`   | Counts accepted command updates per client. Workers apply each id exactly once.                             |
| Reported state | What the worker says it is doing. Separate from the desired command.                                        |

Client lifecycle (reported states):

```
registered -> idle <-> running <-> paused
                 \         |
                  +---> stopping ---> done      (final)
                  +---------------------> error (final)
```

- A finished client (`done` or `error`) keeps its account until the server restarts and cannot be restarted through `/command`.
- Desired command and reported state are independent: after a timed pause expires a worker reports `running` while the desired command is still the already-acknowledged `pause`.
- `target=all` reaches the clients that exist at that moment and have not finished; clients registering later start `idle`.
- Pool exhausted: registration fails with 409 and does not consume a number.

## Build

Requirements: a C++17 compiler (g++ 7+ or newer) and GNU make. On Windows use MinGW/MSYS2 (or the direct MSVC command below).

```bash
make          # builds ./nshtestherd (the product, one binary)
make test     # also builds and runs test_core and test_runner (test programs, not part of the product)
make clean
```

`make test` builds extra programs from `tests/`; they are separate executables and are not part of `nshtestherd`.
For deployment only the `nshtestherd` binary is needed.

Direct compiler builds (product only):

```bash
# Linux
g++ -std=c++17 -O2 -Wall -Wextra -pthread -o nshtestherd src/main.cpp src/http.cpp src/runner.cpp src/httpclient.cpp src/process.cpp src/api.cpp src/herd.cpp src/csv.cpp src/wire.cpp

# Windows (MSVC, Developer prompt)
cl /std:c++17 /EHsc /O2 /Fe:nshtestherd.exe src\main.cpp src\http.cpp src\runner.cpp src\httpclient.cpp src\process.cpp src\api.cpp src\herd.cpp src\csv.cpp src\wire.cpp ws2_32.lib

# Windows (MinGW)
g++ -std=c++17 -O2 -o nshtestherd.exe src/main.cpp src/http.cpp src/runner.cpp src/httpclient.cpp src/process.cpp src/api.cpp src/herd.cpp src/csv.cpp src/wire.cpp -lws2_32 -lshell32 -pthread
```

## Server mode

`nshtestherd [options]` is the default mode. It runs in the foreground; Ctrl+C / SIGTERM shuts down gracefully.
Starting without accounts is valid: registration returns 409 `no_accounts_loaded` until accounts arrive
(`--csv`, `--generate`, or `POST /load`).

| Option                | Default        | Meaning                                                                                                                                        |
| --------------------- | -------------- | ---------------------------------------------------------------------------------------------------------------------------------------------- |
| `--bind <ipv4>`       | `127.0.0.1`    | Listen address (IPv4 literal). Use `0.0.0.0` for remote workers.                                                                               |
| `--port <n>`          | `8788`         | Listen port.                                                                                                                                   |
| `--csv <file>`        | none           | Load accounts at startup.                                                                                                                      |
| `--generate <n>`      | none           | No CSV: simulate one with `n` accounts (1-1000000), e.g. `Load,000001,TestPassword,load000001,load000001@example.com`. Exclusive with `--csv`. |
| `--prefix <name>`     | `load`         | Generated shortname/mail prefix (first letter upper-cased becomes FirstName). Needs `--generate`.                                              |
| `--password <pw>`     | `TestPassword` | Generated password. Needs `--generate`.                                                                                                        |
| `--domain <name>`     | `example.com`  | Generated mail domain. Needs `--generate`.                                                                                                     |
| `--threads <n>`       | `8`            | Worker threads. Up to 64 accepted connections wait in a queue; beyond that clients get 503.                                                    |
| `--max-csv-bytes <n>` | `16777216`     | Body limit for `POST /load`. All other bodies are limited to 64 KB, headers to 16 KB (413).                                                    |
| `--timeout <s>`       | `10`           | Socket timeout and total deadline for reading one request.                                                                                     |
| `--verbose`           | off            | One log line per request: method, path, status and `test_id` when known (e.g. `POST /status 200 test_id=3`). Bodies are never logged.          |
| `--version`, `--help` |                | Print version / usage.                                                                                                                         |

Exit codes: 0 normal stop, 1 startup failure (cannot bind, CSV unreadable or invalid, worker threads cannot be started), 2 usage error.

### Account CSV

Exactly five columns, in the nshreg order, headerless or with an optional
header row:

```csv
Load,000001,TestPassword,load000001,load000001@example.com
```

`FirstName,LastName,Password,Shortname,InternetAddress`. Quoted fields,
`""` escapes, commas and line breaks inside quotes, CRLF/LF, a UTF-8 BOM and
empty fields are supported. Values are kept as-is (passwords are never
trimmed). Wrong column count, empty or duplicate Shortname are rejected with
the record number; an import is validated completely before it replaces the
pool, so a failed import changes nothing. `POST /load` replaces the pool only
until the first client registers (then 409). A sample is in [examples/users.csv](examples/users.csv).

### Generated accounts

`--generate N` builds the same pool a CSV would give, without a file. Names are the prefix plus a zero-padded
number (at least 6 digits): `load000001`, `load000002`, ... with the defaults. The pool is bounded; when it is
booked, registration returns 409 `pool_exhausted`.

### Metrics

`GET /metrics` returns Prometheus text with the prefix `nshtestherd_` (gauges for users and clients by state, counters for
registrations, failures, status reports, command updates and CSV loads). Labels are only the fixed state names. Scrape it with:

```yaml
scrape_configs:
  - job_name: nshtestherd
    static_configs:
      - targets: ["coordinator:8788"]
```

The full metric list is in [docs/PROTOCOL.md](docs/PROTOCOL.md).

## Runner (optional)

The same executable can act as a worker host. `--runner` connects to a separate
coordinator over HTTP (using only the public API) and can run on any test machine.
The server does not need it; any HTTP client can be a worker.

```bash
nshtestherd --csv users.csv --port 8788                                   # coordinator
nshtestherd --runner --server http://coordinator:8788                     # up to 100 clients (fewer if the pool is booked)
nshtestherd --runner --server http://coordinator:8788 --clients 100       # exactly 100 (per machine), built-in dummy job
nshtestherd --runner --server http://coordinator:8788 --clients all       # one client per free account (max 1000), single runner
nshtestherd --runner --server http://coordinator:8788 --clients 100 \
  --program nshload -- --config load.ini                                  # external program
```

| Option               | Default                 | Meaning                                                                                    |
| -------------------- | ----------------------- | ------------------------------------------------------------------------------------------ |
| `--runner`           |                         | Select runner mode.                                                                        |
| `--server <url>`     | `http://127.0.0.1:8788` | Coordinator, `http://host[:port]` (no https).                                              |
| `--clients <n\|all>` | up to `--max-clients`   | Exactly `n` clients (max 1000), or `all` = keep going until the pool is booked (max 1000). |
| `--max-clients <n>`  | `100`                   | Limit for the default mode. Not allowed together with `--clients`.                         |
| `--program <exe>`    | none (dummy job)        | Run this program once per client and `run` command.                                        |
| `-- <args...>`       |                         | Arguments passed unchanged to every launched program. Requires `--program`.                |
| `--poll-seconds <n>` | `2`                     | Status polling interval.                                                                   |
| `--verbose`          | off                     | Accepted for symmetry; the runner always logs its key events.                              |

Server options (`--bind --port --csv --generate --prefix --password --domain --threads --max-csv-bytes --timeout`)
and runner options cannot be mixed. Exit codes: 0 no client failed, 1 at least one client failed (error, refused
registration, coordinator lost), 2 usage error.

How many clients:

- `--clients N` starts exactly N clients (a booked pool then counts as a failure). Run one runner per machine,
  each with its own N; they draw from the same pool, so ids and accounts never overlap.
- Without `--clients` the runner starts up to `--max-clients` (default 100) clients, one registration at a time,
  and stops early when the pool is booked (`pool_exhausted`).
- `--clients all` keeps going until the pool is booked (max 1000). Use it with a single runner, because it takes
  every free account. One thread per client, so a pool above 1000 needs several runners.

Behaviour:

- Each client registers separately (own `request_key`, own sequential `test_id` and account) and tracks its own
  state, command id, acknowledgement and pause timer. Polling continues while idle, working or paused.
- **Dummy job** (no `--program`): `run` marks the client running until superseded. It exercises
  allocation, commands, status reports and the coordinator metrics.
- **External program**: every `run` command starts one child per client, directly (no shell;
  use a real executable, not a `.bat`/`.cmd`). Arguments after `--` are passed unchanged. The
  account and job come in via environment variables:

  | Variable                                                                | Value                                        |
  | ----------------------------------------------------------------------- | -------------------------------------------- |
  | `NSH_TEST_ID`                                                           | sequential test client number                |
  | `NSH_FIRSTNAME`, `NSH_LASTNAME`, `NSH_SHORTNAME`, `NSH_INTERNETADDRESS` | allocated account                            |
  | `NSH_PASSWORD`                                                          | allocated password (environment, never argv) |
  | `NSH_JOB`                                                               | job name from the `run` command              |
  | `NSH_COMMAND_ID`                                                        | command id that started this child           |
  | `NSH_SERVER`                                                            | coordinator URL                              |

- Child exit 0: the client reports `idle` with the message `job X finished (exit 0)`. A finished
  child is **not** restarted for an already acknowledged command id; a new `run` starts a new one.
  Non-zero exit or launch failure: the client reports `error` (final) and the runner exits 1.
- Pause, job changes and graceful stop are applied **between** child executions: while a child
  runs, new instructions wait and are acknowledged once applied. Nothing here pauses or stops an
  arbitrary program mid-run. Ctrl+C on the runner stops the children and reports
  the clients `done` (message `runner interrupted`). Linux: SIGTERM first, SIGKILL after 5 seconds if the
  child ignores it, and the child is always reaped (no zombie or orphan). Windows has no polite stop for
  console programs: the child is terminated immediately.
- **The runner owns status reporting.** For each client the runner is the only one that sends `POST /status`
  (state, acknowledgements, last contact). A child must not report with the same `test_id`: its reports would
  overwrite the runner's state and acknowledgements. A child may read its desired command with
  `GET /client?test_id=$NSH_TEST_ID` (read-only, no password), but it cannot acknowledge it.
- **Cooperative control (not implemented yet).** Live pause/stop of a running child, for example a load program,
  needs that program's cooperation. The intended design keeps the ownership above: the child watches the desired
  command read-only and reports what it did through its exit code or a runner-side channel, while the runner
  stays the single reporter. Until then, long-running children delay pause and stop until they exit.
- One process per logical client suits workloads that need a separate identity per process (for
  example Domino); threads are used only for the runner's own coordination and dummy clients.
- Registration is spread over the first seconds and retried with the same key (503, unreachable server,
  pool not loaded yet, or a reply that is cut off or lacks the account fields: the same key returns the same
  allocation). Replies are checked against their `Content-Length`; a truncated registration or status reply is
  never used.
- If the runner cannot start a client thread (resource limits) it stops the clients already running, joins them
  and exits 1 instead of carrying on with a partial herd. The server does the same if it cannot start its worker
  threads.
- Windows: account values and `--program` arguments are handled as UTF-8 (wide process APIs). The `--csv` path
  must be ASCII on Windows.

### Smoke test without any load program

The runner can start `nshtestherd` itself as the child. `--child-info` prints the child's process id and the
`NSH_*` values it received (never the password) and exits 0:

```bash
./nshtestherd --generate 10
./nshtestherd --runner --clients 2 --program ./nshtestherd -- --child-info
curl -X POST -d 'target=all&command=run&job=demo' http://127.0.0.1:8788/command
# [client 1 ...] child pid=4711 NSH_TEST_ID=1 NSH_SHORTNAME=load000001 NSH_JOB=demo ... NSH_PASSWORD=(set)
```

## Troubleshooting

| Symptom                                           | Cause / fix                                                                                       |
| ------------------------------------------------- | ------------------------------------------------------------------------------------------------- |
| Registration returns 409 `no_accounts_loaded`     | No pool yet. Start with `--csv` / `--generate` or `POST /load`. Runners wait and retry.           |
| Registration returns 409 `pool_exhausted`         | Every account is booked. Restart the server for a fresh herd, or load a larger pool first.        |
| `POST /load` returns 409 `load_not_allowed`       | A client has already registered. Restart the server to load a different pool.                     |
| Runner: "coordinator no longer knows this client" | The server was restarted. Stop the old runners before restarting the server.                      |
| Runner: "coordinator unreachable"                 | Wrong `--server`, server down, or `--bind` is `127.0.0.1` while the runner is on another machine. |
| 503 from the server                               | More than 64 connections waiting. Raise `--threads` or poll less often.                           |
| 413                                               | Request too large (64 KB general, `--max-csv-bytes` for `/load`).                                 |
| Client stays in `error`                           | A child exited non-zero or could not be started; see its `message` in `GET /client?test_id=N`.    |

## Limits and non-goals

- All state in memory. No persistence, leases, or automatic account reclamation after a crashed worker.
- No authentication, no TLS: run it on a trusted test network. `--bind 0.0.0.0` exposes the passwords returned by `/register`.
- IPv4 only; the runner talks `http://` only.
- One request per connection (no keep-alive, no chunked request bodies).
- No Kubernetes integration, no web UI, no user provisioning, no load generation of its own.

## Tests

```bash
make test                            # builds and runs test_core and test_runner
tests/integration.sh ./nshtestherd   # HTTP layer with curl (default port 18788, set HERD_TEST_PORT)
```

| Test             | What it covers                                                                                                                                                                                                                                                                                                      |
| ---------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `test_core`      | CSV quoting and rollback, wire format, `--generate`, allocation and retry keys, exhaustion, 16-thread concurrency, commands vs. state, metrics (no sockets)                                                                                                                                                         |
| `test_runner`    | Runner against an in-process coordinator on `127.0.0.1:18790` (`HERD_TEST_PORT`; skipped if busy): dummy clients, fill mode and cap, `/bin/sh` child, failing job, missing program, self-child (`--child-info`), reply framing (truncated and chunked replies rejected), a SIGTERM-ignoring child killed and reaped |
| `integration.sh` | Real HTTP: limits (413), methods (405), chunked (501), CSV over the wire, retry keys, metrics, clean shutdown                                                                                                                                                                                                       |

`test_core`, `test_runner` and `integration.sh` are not part of the product binary.
All three use the standard section headers (a 90-character rule above and below the title). Each test prints one result line and the final summary is a line starting with `[ OK ]` or `[ FAIL ]` (a failing check also prints `[ FAIL ] file:line: condition`).
`test_runner` shows what to expect under each test name, and the runner's own log lines appear in between. Two
scenarios fail **on purpose** (a job that exits 1, a program that cannot be started), so `1 failed` in the runner log
there is the expected result. Only a `[ FAIL ]` line or a non-zero exit of `make test` means a problem.
The `/bin/sh` scenarios are POSIX only; the self-child scenario also runs on Windows.

## Layout

```
src/platform.h    socket portability (Winsock / POSIX)
src/csv.*         account import and --generate
src/wire.*        flat fields, text/JSON rendering, form decoding
src/herd.*        in-memory state, one mutex, no socket access
src/api.*         routing and request validation (testable without sockets)
src/http.*        bounded-thread HTTP/1.1 listener, one request per connection
src/runner.*      optional runner: logical clients over the HTTP API
src/httpclient.*  minimal HTTP client (runner only)
src/process.*     direct program launch, no shell (runner only)
src/main.cpp      command line, signals
tests/            test_core, test_runner, integration.sh
examples/         users.csv, worker.sh (Bash worker, no Domino calls)
docs/PROTOCOL.md  HTTP API and worker contract
```
