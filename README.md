# nshtestherd

**nshtestherd** gives each of your test processes its own test user and controls them all from one place. You load your
test accounts once; every worker that registers gets the next free account and a sequential test number, polls for
commands (`run`, `pause`, `stop`), reports what it is doing, and Prometheus shows how the run is going.

It does **not** create users and does **not** generate load itself: it coordinates the processes that do. It has no Domino
dependency, and any HTTP client (C/C++, LotusScript, Bash, k6, curl) can be a worker. One small binary, no external
libraries, all state in memory.

A **herd** is one test run as the coordinator sees it: the account pool plus all clients registered against it.

> **Security:** no TLS, and `/register` hands out account passwords. An optional shared token (`NSHTEST_TOKEN`) keeps
> strangers from steering the herd; without it anyone who reaches the port can. Run it on a trusted test network, see
> [Security](#security).

**Status:** version 0.9.1. Tested on Linux; the Windows build is written but not yet built or tested
(see [Status and limits](#status-and-limits)).

One executable, two modes:

```
  ┌──────────────────────┐            ┌────────────────────────────────┐                                  ┌────────────────────────────────┐
  │  Prometheus          │            │  nshtestherd                   │                                  │  nshtestherd --runner          │
  │                      │            │  server mode (default)         │ HTTP: register, status, command  │  optional worker host          │
  │                      │    GET     │                                │      (see docs/PROTOCOL.md)      │                                │
  │  scrapes /metrics    │ ──────────►│  • account pool                │◄──────────────────────────────── │  • N logical clients           │
  │                      │            │  • clients and commands        │                                  │  • built-in dummy job, or      │
  │                      │            │  • /metrics                    │                                  │    one child per client        │
  │                      │            │                                │                                  │                                │
  └──────────────────────┘            └────────────────────────────────┘                                  └────────────────────────────────┘
                                                       ▲
                                                       │
                                                       │  any other HTTP client works the same way
                                ┌────────────────────────────────────────────┐
                                │  Your own workers                          │
                                │  Bash · k6 · LotusScript · C/C++ · curl    │
                                └────────────────────────────────────────────┘
```

## Contents

- [Quick start](#quick-start) - running in five minutes
- [Typical test run](#typical-test-run) - the admin workflow from start to finish
- [Operator cheat sheet](#operator-cheat-sheet) - the commands you will use during a test
- [Concepts](#concepts) - accounts, clients, commands, states
- [Run with Docker](#run-with-docker) - container image and Docker Compose stack
- [Build](#build) - from source
- [Server mode](#server-mode) - options, account files, metrics, sizing
- [Runner (optional)](#runner-optional) - a worker host that launches your test program
- [Security](#security), [Troubleshooting](#troubleshooting), [Status and limits](#status-and-limits)
- [Tests](#tests), [Repository layout](#repository-layout), [Releasing](#releasing-maintainers), [License](#license)

The HTTP API with a curl example for every call is in [docs/PROTOCOL.md](docs/PROTOCOL.md). Writing a program that the runner
starts (identity, arguments, exit codes, cooperative stop): [docs/RUNNER-PROGRAMS.md](docs/RUNNER-PROGRAMS.md).

## Quick start

**1. Start the coordinator** with 20 generated test users (use `--csv your-users.csv` for your own accounts):

```bash
docker run -d --name herd -p 8788:8788 ghcr.io/nashcom/nshtestherd --bind 0.0.0.0 --generate 20
```

or from source (see [Build](#build)):

```bash
make
./nshtestherd --generate 20
```

**2. Check that it is up:**

```bash
curl http://127.0.0.1:8788/health
```

```ini
status=ok
```

**3. Register a test worker.** This is what every worker does first; it gets the next free account and a `test_id`:

```bash
curl -X POST -d request_key=my-worker-1 http://127.0.0.1:8788/register
```

```ini
test_id=1
firstname=Load
lastname=000001
password=TestPassword
shortname=load000001
internetaddress=load000001@example.com
command=idle
command_id=0
job=
params=
pause_seconds=0
worker_options=
```

**4. Tell all clients to run a job**, then look at the client:

```bash
curl -X POST -d 'target=all&command=run&job=demo' http://127.0.0.1:8788/command
curl 'http://127.0.0.1:8788/client?test_id=1'
```

```ini
test_id=1
shortname=load000001
state=registered
ack_command_id=0
last_contact_seconds=3
message=
command=run
command_id=1
job=demo
params=
pause_seconds=0
```

The *desired* command is `run`, but the worker has not reported yet (`state=registered`, `ack_command_id=0`). A worker
polls `POST /status`, applies the command and acknowledges it; the two sides are tracked separately.

**5. Let the built-in runner do the worker part.** It starts 5 clients that register, follow commands and run a small
child program (here the binary itself, which prints its process id and the account it received):

```bash
./nshtestherd --runner --clients 5 --program ./nshtestherd -- --child-info
curl -X POST -d 'target=all&command=run&job=demo' http://127.0.0.1:8788/command
```

(`./examples/worker.sh` is the same idea as a plain Bash script: it registers, polls, applies each command once and
supports a timed pause.) Next: [Typical test run](#typical-test-run), and the full API in
[docs/PROTOCOL.md](docs/PROTOCOL.md).

**Prefer to watch it run?** `examples/k6/run.sh` does all of this hands-free in containers: it starts the coordinator and
k6 workers, drives a complete run, pause and stop, checks every step and cleans up (needs Docker and curl; see
[examples/k6](examples/k6/)).

## Typical test run

1. **Prepare the accounts.** The users must exist in the system you test, with the passwords in the file. Use your
   existing nshreg CSV (`--csv users.csv`, see [Account CSV](#account-csv)) or let the coordinator generate names
   (`--generate N`).
2. **Start the coordinator** on a machine the test machines can reach: `--bind 0.0.0.0` (or the Docker/Compose
   [stack](#docker-compose-stack)). Open the port only for your test network.
3. **Start the workers.** On every test machine start a runner that launches your test program once per client, for
   example `nshtestherd --runner --server http://coordinator:8788 --clients 50 --program mytests -- --config mytests.ini`
   (see [Runner](#runner-optional)). Or let your own workers call the API, for example the [k6 worker example](examples/k6/) (it runs in a container).
4. **Check that everyone is registered:** `curl http://coordinator:8788/status` and compare `clients_total` with what you
   expect (and `users_available` with what is left).
5. **Start the test:** send `run` to all clients and watch `/status`, `/metrics` (Prometheus/Grafana) or a single
   client with `/client?test_id=N`.
6. **Steer:** `pause` for a number of seconds, `run` a different job, or `stop`. Each command reaches a worker the next
   time it polls.
7. **Finish and reset.** Stopped workers report `done`. Their accounts stay reserved, so to run again restart the
   coordinator (a fresh herd with numbering from 1 and every account free). Stop the old workers first.

## Operator cheat sheet

Set `H` once (use the coordinator's host name when you are not on the same machine):

```bash
H=http://127.0.0.1:8788
```

| Goal                         | Command                                                                     |
| ---------------------------- | --------------------------------------------------------------------------- |
| Is it up?                    | `curl $H/health`                                                            |
| Herd summary                 | `curl $H/status`                                                            |
| Job results so far           | `curl -s $H/status \| grep '^jobs_'`                                        |
| One client (and its last job)| `curl "$H/client?test_id=1"`                                                |
| Run a job on all             | `curl -X POST -d 'target=all&command=run&job=NAME' $H/command`              |
| Run a job on one client      | `curl -X POST -d 'test_id=3&command=run&job=NAME' $H/command`               |
| Run a job with parameters    | `curl -X POST -d 'target=all&command=run&job=NAME' --data-urlencode 'params=a=1&b=2' $H/command` |
| End the jobs, keep workers   | `curl -X POST -d 'target=all&command=idle' $H/command`                      |
| Pause all for 60 s           | `curl -X POST -d 'target=all&command=pause&pause_seconds=60' $H/command`    |
| Stop all (workers end)       | `curl -X POST -d 'target=all&command=stop' $H/command`                      |
| Stop one client              | `curl -X POST -d 'test_id=3&command=stop' $H/command`                       |
| Load another pool            | `curl -X POST -H 'Content-Type: text/csv' --data-binary @users.csv $H/load` |
| Metrics                      | `curl $H/metrics`                                                           |
| Start over                   | Restart the coordinator (stop the workers first)                            |

`/load` only works before the first client has registered. Add `-i` to any curl call to see the HTTP status, and
`-H 'Accept: application/json'` to get JSON instead of `key=value` lines.

**With a token** (the coordinator runs with `NSHTEST_TOKEN`), every call but `/health` needs the header. Put it in a
variable once and add it to each call:

```bash
A="Authorization: Bearer $NSHTEST_TOKEN"
curl -H "$A" $H/status
curl -H "$A" -X POST -d 'target=all&command=run&job=NAME' $H/command
```

A job that ends - finished, failed, or replaced by `idle`, another `run` or `stop` - never ends the worker: it reports the
result (`last_job_result` in `/client`, the `jobs_ok`, `jobs_failed`, `jobs_stopped` totals in `/status`) and is `idle`
again, waiting for the next command. Only `stop` ends a worker.

curl is the operator's tool and runs wherever you are: any machine that reaches the coordinator's port. The workers do
not need it; they talk to the coordinator themselves (polling), and the coordinator never connects to them.

## Concepts

| Term           | Meaning                                                                                        |
| -------------- | ---------------------------------------------------------------------------------------------- |
| Account pool   | The imported or generated users, in order. Each account is handed out once.                    |
| Herd           | One test run: the account pool plus all clients registered against it.                         |
| Client         | One registered worker: the next free account plus the next `test_id` (1, 2, 3, ...).           |
| `test_id`      | Sequential client number from the server. Never reused in one run; unrelated to account names. |
| `request_key`  | Optional key a worker generates once and repeats on retries: the same allocation comes back.   |
| Command        | What the coordinator wants: `idle`, `run <job>`, `pause <seconds>`, `stop`.                    |
| `command_id`   | Counts accepted command updates per client. Workers apply each id exactly once.                |
| Reported state | What the worker says it is doing. Separate from the desired command.                           |

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

## Run with Docker

Prefer not to build from source? The release publishes a container image, and the repository contains a Docker Compose
stack with the coordinator and an optional runner.

### Container image

The release workflow publishes a multi-arch image (linux/amd64 and linux/arm64) to the GitHub Container Registry:

```bash
docker pull ghcr.io/nashcom/nshtestherd:latest     # newest release
docker pull ghcr.io/nashcom/nshtestherd:0.9.1      # a specific release
```

| Item        | Value                                                                                           |
| ----------- | ----------------------------------------------------------------------------------------------- |
| Image       | `ghcr.io/nashcom/nshtestherd` with tags `<version>` and `latest`                                |
| Platforms   | `linux/amd64`, `linux/arm64` (one manifest list, Docker picks the right one)                    |
| Base        | `FROM scratch`: only the static binary (Alpine/musl build), no OS, no shell, no package manager |
| Contents    | `/nshtestherd` and an empty `/tmp`                                                              |
| User        | `1000:1000` (not root)                                                                          |
| Entrypoint  | `/nshtestherd`: every argument you pass to `docker run` goes to the program                     |
| Port        | `8788` (`EXPOSE`; publish it with `-p 8788:8788` for the server)                                |
| Healthcheck | none (there is no shell or curl in the image); check `GET /health` from outside                 |
| State       | none: everything is in memory, a container restart starts a fresh herd                          |

The same static binary is attached to each GitHub release as `nshtestherd-<version>-amd64` and `-arm64` (with `.sha256`
files), so you can run it without Docker.

**Server.** Inside a container the server must listen on `0.0.0.0`; the default `127.0.0.1` is not reachable from outside:

```bash
docker run -d --name herd -p 8788:8788 ghcr.io/nashcom/nshtestherd --bind 0.0.0.0 --generate 100
curl http://127.0.0.1:8788/health
docker logs herd
docker stop herd          # SIGTERM: graceful shutdown
```

To use your own accounts, mount the CSV read-only (the container user must be able to read the file):

```bash
docker run -d --name herd -p 8788:8788 -v "$PWD/users.csv:/users.csv:ro" \
  ghcr.io/nashcom/nshtestherd --bind 0.0.0.0 --csv /users.csv
```

**Runner.** The same image is the runner. On a user-defined Docker network containers reach each other by name:

```bash
docker network create herdnet
docker run -d --name herd --network herdnet -p 8788:8788 ghcr.io/nashcom/nshtestherd --bind 0.0.0.0 --generate 100
docker run --rm --network herdnet ghcr.io/nashcom/nshtestherd --runner --server http://herd:8788 --clients 10
```

On a different machine use `--server http://<host>:8788`. Stop a runner with Ctrl+C or `docker stop` (clients report `done`).

**Smoke test with the image only.** The binary can start itself as the child program, so no load program is needed:

```bash
docker run --rm --network herdnet ghcr.io/nashcom/nshtestherd --runner --server http://herd:8788 --clients 2 \
  --program /nshtestherd -- --child-info
curl -X POST -d 'target=all&command=run&job=demo' http://127.0.0.1:8788/command
```

### Docker Compose stack

[docker-compose.yml](docker-compose.yml) is a ready-to-run stack: the coordinator (`herd`) and an optional runner (`runner`)
that registers clients against it. It builds the image from the `Dockerfile` in this repository. The herd is the product and
starts by default; the runner is a test tool in the compose profile `test` and only starts when you ask for it.

```bash
docker compose up -d --build                  # the herd only (use your own workers), in the background
docker compose --profile test up --build      # the herd and a runner (foreground, Ctrl+C stops both)
docker compose up runner                      # naming the runner starts it too (and the herd)
```

In another shell, drive the herd (the port is published on `127.0.0.1:8788`):

```bash
curl http://127.0.0.1:8788/status
curl -X POST -d 'target=all&command=run&job=demo' http://127.0.0.1:8788/command
curl http://127.0.0.1:8788/metrics
docker compose logs -f runner
```

| Service  | What it does                                                                   | Profile | Restart policy |
| -------- | ------------------------------------------------------------------------------ | ------- | -------------- |
| `herd`   | Coordinator: `--bind 0.0.0.0 --port --generate`, port published on the host    | (none)  | `always`       |
| `runner` | `--runner --server http://herd:<port> --clients <n>`, reaches the herd by name | `test`  | `no`           |

The herd always comes back after a Docker or host restart (but a restarted herd is a fresh herd, see below). The runner is
not restarted automatically: a finished runner (stop command) or a failed one would otherwise register a new set of clients
and use up accounts.

Settings, from the shell environment or a `.env` file next to `docker-compose.yml`:

| Variable            | Default              | Meaning                                                          |
| ------------------- | -------------------- | ---------------------------------------------------------------- |
| `HERD_USERS`        | `100`                | Generated accounts (`load000001` ...); the pool is bounded       |
| `RUNNER_CLIENTS`    | `10`                 | Logical clients per runner container                             |
| `HERD_PORT`         | `8788`               | Port, on the host and in the container                           |
| `HERD_BIND`         | `127.0.0.1`          | Host address the port is published on (`0.0.0.0`: whole network) |
| `NSHTESTHERD_IMAGE` | `nshtestherd:latest` | Image name (default: built locally; or the published image)      |

```bash
HERD_USERS=500 RUNNER_CLIENTS=50 docker compose --profile test up -d --build
```

**Use the published image instead of building:**

```bash
NSHTESTHERD_IMAGE=ghcr.io/nashcom/nshtestherd:latest docker compose pull
NSHTESTHERD_IMAGE=ghcr.io/nashcom/nshtestherd:latest docker compose up -d
```

**More runners.** Scale the runner service; each container runs its own `RUNNER_CLIENTS` clients and draws from the same
pool, so ids and accounts never overlap. Keep the total within the pool, otherwise the runners that find it booked report
failures:

```bash
docker compose --profile test up -d --scale runner=3   # 3 x RUNNER_CLIENTS clients, needs HERD_USERS >= that
docker compose ps
```

**Your own accounts.** Put the CSV next to the compose file and, in `docker-compose.yml`, replace the `--generate` lines in
the `herd` command with the two commented `--csv` lines, and enable the `volumes` entry (the container user, uid 1000, must be
able to read `users.csv`):

```yaml
    command:
      - "--bind"
      - "0.0.0.0"
      - "--port"
      - "${HERD_PORT:-8788}"
      - "--csv"
      - "/users.csv"

    volumes:
      - ./users.csv:/users.csv:ro
```

**Smoke test without a load program.** Enable the commented `--program /nshtestherd -- --child-info` lines in the `runner`
command. After a `run` command each client's child prints `child pid=... NSHTEST_SHORTNAME=...` in `docker compose logs runner`.
A real load program needs a derived image (see above); put its image name in `NSHTESTHERD_IMAGE` or in a `build:` of your own.

**Runners on other machines.** Publish the herd on the network with `HERD_BIND=0.0.0.0` and set a token (trusted network
only: there is no TLS and `/register` returns passwords), then start a runner on the other machine with the same token:

```bash
export NSHTEST_TOKEN=$(openssl rand -hex 24)
HERD_BIND=0.0.0.0 docker compose up -d herd
docker run --rm -e NSHTEST_TOKEN ghcr.io/nashcom/nshtestherd --runner --server http://<herd-host>:8788 --clients 20
```

**Stopping and restarting.** `docker compose --profile test down` removes the containers, including the runner (without
`--profile test` only the herd is removed); the next `up` starts a fresh herd (numbering from 1, all accounts free).
Restarting only the herd (`docker compose restart herd`, or Docker restarting it) also resets it, and the running runners then
lose their clients, so restart or recreate the runners with it: `docker compose --profile test up -d --force-recreate`. Workers can be
stopped cleanly with `curl -X POST -d 'target=all&command=stop' http://127.0.0.1:8788/command`: the clients report `done`
and the runner container exits with code 0.

| Symptom                                      | Cause / fix                                                          |
| -------------------------------------------- | -------------------------------------------------------------------- |
| Runner exits at once: "no free account left" | `HERD_USERS` is smaller than all runners' clients together; raise it |
| Runner: "coordinator unreachable"            | Herd not up yet, or `HERD_PORT` differs; check `docker compose ps`   |
| Port already in use on `up`                  | Another service uses the port; set a different `HERD_PORT`           |
| `docker compose up` starts no runner         | By design (profile `test`): use `--profile test` or `up runner`      |
| Runner on another machine cannot connect     | Herd is published on `127.0.0.1` only; use `HERD_BIND=0.0.0.0`       |
| Herd "running" but not answering             | No container healthcheck; try `curl http://127.0.0.1:8788/health`    |

**Running a real load program from the runner.** The base image contains nothing except `nshtestherd`, so a load program must
be added in a derived image, and it has to run in that image (a static binary, or add the libraries it needs):

```dockerfile
FROM alpine:latest
COPY --from=ghcr.io/nashcom/nshtestherd:latest /nshtestherd /usr/local/bin/nshtestherd
COPY mytests /usr/local/bin/mytests
USER 1000:1000
ENTRYPOINT ["/usr/local/bin/nshtestherd"]
```

```bash
docker run --rm --network herdnet my-runner --runner --server http://herd:8788 --clients 10 \
  --program /usr/local/bin/mytests -- --config /etc/mytests.ini
```

The runner passes the account to each `mytests` process in `NSHTEST_*` environment variables (see [Runner](#runner-optional)).

**Security.** There is no TLS, and `/register` returns the account passwords. Publish the port only on a trusted test
network (for example `-p 127.0.0.1:8788:8788` when only local tools need it), and set `NSHTEST_TOKEN` (`-e NSHTEST_TOKEN`)
as soon as other machines can reach it.

**Building the image yourself:**

```bash
./build.sh                                    # image nshtestherd:latest, static binary extracted to ./nshtestherd
IMAGE_TAG=myregistry/nshtestherd:dev ./build.sh   # different tag
docker build -t nshtestherd:latest .          # image only
```

The build is a two-stage `Dockerfile`: an Alpine stage compiles the static binary with `docker/compile_alpine_static.sh`
(size-optimised, LTO, `--gc-sections`; `docker/fortify_shim.cpp` supplies glibc-only fortify symbols that musl's static libc
lacks), and the `scratch` stage copies in just the binary.

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
Without `--csv` or `--generate` it generates **100 accounts** (`load000001` ... with the password `TestPassword`), so a
coordinator works without any option; `POST /load` can still replace them until the first registration. To start with
an empty pool use `--generate 0`: registration then returns 409 `no_accounts_loaded` until `POST /load` brings accounts.

| Option                | Default        | Meaning                                                               |
| --------------------- | -------------- | --------------------------------------------------------------------- |
| `--bind <ipv4>`       | `127.0.0.1`    | Listen address (IPv4). `0.0.0.0` for remote workers                   |
| `--port <n>`          | `8788`         | Listen port                                                           |
| `--csv <file>`        | none           | Load accounts at startup                                              |
| `--generate <n>`      | `100`          | No CSV: generate `n` accounts (0-1000000; 0: empty pool); not with `--csv` |
| `--prefix <name>`     | `load`         | Generated name prefix (`load000001`); not with `--csv`                |
| `--password <pw>`     | `TestPassword` | Generated password; not with `--csv`                                  |
| `--domain <name>`     | `example.com`  | Generated mail domain; not with `--csv`                               |
| `--worker-options <s>`| none           | Options for the workers, returned with every registration (`name=value&name`); each worker takes what it knows (domlem: `switch`) |
| `--threads <n>`       | `8`            | Worker threads; 64 more connections may wait, then 503                |
| `--max-csv-bytes <n>` | `16777216`     | Size limit for `POST /load` (other bodies 64 KB, headers 16 KB)       |
| `--timeout <s>`       | `10`           | Socket timeout and deadline for reading one request                   |
| `--verbose`           | off            | Log one line per request (method, path, status, `test_id`); no bodies |
| `--version`, `--help` |                | Print version / usage                                                 |

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

Useful queries:

```
nshtestherd_clients_by_state{state="running"}          # clients running right now
sum(nshtestherd_clients_by_state)                      # all registered clients
nshtestherd_users_available                            # accounts still free
rate(nshtestherd_status_reports_total[1m])             # status reports per second (polling load)
nshtestherd_clients_by_state{state="error"} > 0        # alert: a client that cannot work (identity setup failed)
rate(nshtestherd_jobs_ended_total{result="failed"}[5m]) # failed jobs per second
```

### Sizing and operation

- **Not load-tested yet.** Treat any client count above a few hundred as something to try first, and tell us what you find.
- **Load profile.** Requests are tiny and handled in microseconds. The main load is polling: `N` clients polling every
  2 seconds (the runner default, `--poll-seconds`) is about `N / 2` requests per second. Use a longer interval for very
  large herds.
- **Limits.** `--threads` workers (default 8) serve requests; up to 64 more connections wait in a queue and further ones get
  `503`, which the runner retries. Raise `--threads` before raising the polling rate. State is in memory, a few hundred
  bytes per client.
- **Running it.** The server runs in the foreground and logs to stdout/stderr; stop it with Ctrl+C or SIGTERM (graceful).
  As a service use Docker/Compose (`restart: always`) or your own service manager. A restart is a fresh herd.

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
  --program mytests -- --user {NSHTEST_SHORTNAME} --id {NSHTEST_TEST_ID}            # external program, per-client arguments
```

| Option               | Default                 | Meaning                                                         |
| -------------------- | ----------------------- | --------------------------------------------------------------- |
| `--runner`           |                         | Select runner mode                                              |
| `--server <url>`     | `http://127.0.0.1:8788` | Coordinator, `http://host[:port]` (no https)                    |
| `--clients <n\|all>` | up to `--max-clients`   | Exactly `n` (max 1000), or `all`: until the pool is booked      |
| `--max-clients <n>`  | `100`                   | Limit for the default mode; not with `--clients`                |
| `--program <exe>`    | none (dummy job)        | Start this program once per client and `run` command            |
| `-- <args...>`       |                         | Arguments passed unchanged to every program (needs `--program`) |
| `--poll-seconds <n>` | `2`                     | Status polling interval                                         |
| `--verbose`          | off                     | Accepted for symmetry; the runner always logs key events        |

Server options (`--bind --port --csv --generate --prefix --password --domain --threads --max-csv-bytes --timeout`)
and runner options cannot be mixed. Exit codes: 0 no client failed, 1 at least one client failed (refused
registration, coordinator lost), 2 usage error. A failed job is not a failed client.

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
  use a real executable, not a `.bat`/`.cmd`). Arguments after `--` go to every child, with placeholders replaced
  per client (see below). The account and job **always** come in via environment variables:

  | Variable                                                         | Value                                          |
  | ---------------------------------------------------------------- | ---------------------------------------------- |
  | `NSHTEST_TEST_ID`                                                | sequential test client number                  |
  | `NSHTEST_FIRSTNAME`, `NSHTEST_LASTNAME`                          | allocated account: names                       |
  | `NSHTEST_SHORTNAME`, `NSHTEST_INTERNETADDRESS`                   | allocated account: short name and mail address |
  | `NSHTEST_PASSWORD`                                               | allocated password (environment, never argv)   |
  | `NSHTEST_JOB`                                                    | job name from the `run` command                |
  | `NSHTEST_PARAMS`                                                 | parameters of the `run` command (may be empty) |
  | `NSHTEST_COMMAND_ID`                                             | command id that started this child             |
  | `NSHTEST_SERVER`                                                 | coordinator URL                                |
  | `NSHTEST_TOKEN`                                                  | inherited from the runner, when it has a token |

  **Renamed:** these variables used to be called `NSH_*` (`NSH_SHORTNAME`, `NSH_PASSWORD`, ...). The runner still sets the
  old names as well, and `{NSH_...}` placeholders still work, so existing programs keep running. Both are deprecated and
  go away in a later release: switch to `NSHTEST_*`.

- **Argument placeholders.** To give every child its own command-line arguments, use the variable names in braces in the
  arguments after `--`. The placeholders are exactly the environment variable names above, so there is nothing new to learn:

  ```bash
  nshtestherd --runner --clients 50 --program mytests -- --user {NSHTEST_SHORTNAME} --id {NSHTEST_TEST_ID} --scenario {NSHTEST_JOB}
  ```

  For client 7 the child is started as `mytests --user load000007 --id 7 --scenario <job of the run command>`.
  - Available: `{NSHTEST_TEST_ID}`, `{NSHTEST_FIRSTNAME}`, `{NSHTEST_LASTNAME}`, `{NSHTEST_SHORTNAME}`, `{NSHTEST_INTERNETADDRESS}`,
    `{NSHTEST_JOB}`, `{NSHTEST_PARAMS}`, `{NSHTEST_COMMAND_ID}`, `{NSHTEST_SERVER}`.
  - There is no shell: a placeholder is replaced inside its own argument, so a value with spaces stays one argument.
  - Everything that is not a complete `{NSHTEST_NAME}` is literal text, so JSON such as `{"a":1}` needs no escaping.
  - An unknown name (for example `{NSHTEST_TYPO}`) is refused at startup with exit code 2, not when the first child starts.
  - **`{NSHTEST_PASSWORD}` is refused:** an argument is visible to every user in the process list (`ps`, `/proc`). A program
    reads the password from the environment variable `NSHTEST_PASSWORD` instead.
  - Do not write `$NSHTEST_SHORTNAME` or `${NSHTEST_SHORTNAME}`: your shell, or Docker Compose in a `command:` list, expands that
    itself before the runner sees it. Use the braces alone (`{NSHTEST_SHORTNAME}`; they are safe in bash and in Compose).
  - Values are inserted as they are and are never expanded a second time.

- Child exit 0: the job is reported as `ok` with the message `job X finished (exit 0)`. A finished
  child is **not** restarted for an already acknowledged command id; a new `run` starts a new one.
  Non-zero exit or launch failure: the job is reported as `failed` with the message `job X failed (exit N)`.
  Either way the client goes back to `idle` and waits for the next command; a job never ends the client.
  The coordinator keeps the result per client (`GET /client`: `last_job_id`, `last_job`, `last_job_result`,
  `last_job_message`) and counts it (`GET /status`: `jobs_ok`, `jobs_failed`, `jobs_stopped`).
- Pause, job changes and graceful stop are applied **between** child executions: while a child
  runs, new instructions wait and are acknowledged once applied. Nothing here pauses or stops an
  arbitrary program mid-run. Ctrl+C on the runner stops the children and reports
  the clients `done` (message `runner interrupted`). Linux: SIGTERM first, SIGKILL after 5 seconds if the
  child ignores it, and the child is always reaped (no zombie or orphan). Windows has no polite stop for
  console programs: the child is terminated immediately.
- **The runner owns status reporting.** For each client the runner is the only one that sends `POST /status`
  (state, acknowledgements, last contact). A child must not report with the same `test_id`: its reports would
  overwrite the runner's state and acknowledgements. A child may read its desired command with
  `GET /client?test_id=$NSHTEST_TEST_ID` (read-only, no password), but it cannot acknowledge it.
- **Cooperative stop for long-running programs.** The runner cannot pause or stop a program in the middle of its work; the
  program has to cooperate. It can do that today: it reads the desired command read-only with
  `GET /client?test_id=$NSHTEST_TEST_ID`, and when it sees `stop` (or `pause`) it finishes its current operation and exits
  with 0. The runner then applies the pending command and reports it. The runner stays the only status reporter. A ready-made
  helper library is not provided yet. The pattern, an example script and the exit code rules are in
  [docs/RUNNER-PROGRAMS.md](docs/RUNNER-PROGRAMS.md).
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
`NSHTEST_*` values it received (never the password) and exits 0:

```bash
./nshtestherd --generate 10
./nshtestherd --runner --clients 2 --program ./nshtestherd -- --child-info
curl -X POST -d 'target=all&command=run&job=demo' http://127.0.0.1:8788/command
# [client 1 ...] child pid=4711 NSHTEST_TEST_ID=1 NSHTEST_SHORTNAME=load000001 NSHTEST_JOB=demo ... NSHTEST_PASSWORD=(set)
```

### Domino workers: domlem

[domlem](domlem/) is the Domino add-in version of a runner: a servertask that registers with the coordinator, receives a
test account and does Notes work while following `run`, `pause`, `idle` and `stop`. Jobs: `dbopen`, `agent` (or
`agent:<name>` for any agent of the test database), `mail` and `mailtest` (random subjects, attachments, body text and
recipients from the test pool, with a sent copy). It uses the same client core as the runner (`src/herdclient.*`), so it
behaves the same way. With `-switch` it registers the Domino user (with mail file) if it does not exist, downloads its ID
from the ID vault and works as that user. One process is one client; start it many times for many clients.

```bash
curl -X POST -d 'target=all&command=run&job=mailtest' $H/command
```

See [domlem/README.md](domlem/README.md), with its own curl cheat sheet.

## Security

nshtestherd is a test tool for a trusted network. Know what an open port means:

- **Optional token, no TLS.** Without a token anyone who can reach the port can register (and receive an account **with
  its password**), read the status, issue commands such as `stop` for every client, and load a new account pool before
  the first registration. With `NSHTEST_TOKEN` set (16-256 printable characters, from the environment so it is not in
  the process list) every call but `GET /health` needs `Authorization: Bearer <token>`; the runner, domlem and the
  examples read the same `NSHTEST_TOKEN` and send it. The token travels in plain text: it keeps strangers out, not
  someone who can read the traffic. Generate one with `openssl rand -hex 24`.
- **Safe defaults.** The server listens on `127.0.0.1`; the Compose stack publishes on `127.0.0.1`. Use `--bind 0.0.0.0` /
  `HERD_BIND=0.0.0.0` only on a test network, and limit the port (8788 by default) with a firewall to the test machines.
- **Across untrusted networks** put it behind an SSH tunnel or a TLS reverse proxy with authentication of your choice.
- **Use test accounts only,** never real users. Passwords are kept in memory, are never written to logs (`--verbose` logs
  method, path, status and `test_id` only, no bodies) and are not in metrics.
- **Child programs** started by the runner receive the account in environment variables (never on the command line, so
  not in `ps`). The same OS user and root can read a process environment.
- **Container:** the image runs as user 1000 from `scratch` (no shell, no packages).

## Troubleshooting

| Symptom                               | Cause / fix                                                               |
| ------------------------------------- | ------------------------------------------------------------------------- |
| Registration 409 `no_accounts_loaded` | Started with `--generate 0`: `POST /load` the accounts; runners wait      |
| Registration 409 `pool_exhausted`     | All accounts booked: restart the server, or load a larger pool            |
| `POST /load` 409 `load_not_allowed`   | A client already registered: restart the server to load another pool      |
| Runner: "no longer knows this client" | The server was restarted: stop old runners before restarting it           |
| Runner: "coordinator unreachable"     | Wrong `--server`, server down, or `--bind 127.0.0.1` with a remote runner |
| 401 `unauthorized`                    | The coordinator has a token: send `Authorization: Bearer <token>` (runner: `NSHTEST_TOKEN`) |
| 503 from the server                   | More than 64 waiting connections: raise `--threads` or poll less often    |
| 413                                   | Request too large (64 KB; `--max-csv-bytes` for `/load`)                  |
| Cannot bind: "Address already in use" | Another process uses the port: stop it or set `--port`                    |
| Want to start over                    | Restart the coordinator (stop the workers first): a fresh herd            |

## Status and limits

| Area                                     | State                                                                 |
| ---------------------------------------- | --------------------------------------------------------------------- |
| Server, runner, CSV import, `--generate` | Complete                                                              |
| Tested on                                | Linux (g++, GNU make): all test suites pass                           |
| Windows build                            | Written (Winsock, `CreateProcess`), **not yet built or tested**       |
| Not tested                               | Heavy load (503 on queue overflow, slow clients, 1000 runner clients) |

- All state in memory. No persistence, leases, or automatic account reclamation after a crashed worker.
- No TLS, and authentication is one optional shared token: run it on a trusted test network. `--bind 0.0.0.0` exposes the
  passwords returned by `/register` to anyone who reaches the port (and has the token, when one is set).
- IPv4 only; the runner talks `http://` only.
- One request per connection (no keep-alive, no chunked request bodies).
- No Kubernetes integration, no web UI, no user provisioning, no load generation of its own.


## Tests

```bash
make test                            # builds and runs test_core and test_runner
tests/integration.sh ./nshtestherd   # HTTP layer with curl (default port 18788, set HERD_TEST_PORT)
```

| Test             | What it covers                                                                         |
| ---------------- | -------------------------------------------------------------------------------------- |
| `test_core`      | CSV, wire format, `--generate`, allocation, exhaustion, concurrency, commands, metrics |
| `test_runner`    | Runner against an in-process coordinator: dummy clients, fill and cap, child programs  |
| `integration.sh` | Real HTTP with curl: limits, methods, CSV, retry keys, metrics, shutdown               |

`test_runner` scenarios: dummy clients, fill mode and client cap, argument placeholders, a `/bin/sh` child with a failing job, a program that
cannot be started, the self-child (`--child-info`), reply framing (truncated and chunked replies are rejected) and a
child that ignores SIGTERM (killed and reaped). It uses `127.0.0.1:18790` (`HERD_TEST_PORT`) and is skipped if that
port is busy.

`test_core`, `test_runner` and `integration.sh` are not part of the product binary.
All three use the standard section headers (a 90-character rule above and below the title). Each test prints one result
line, and the final summary line starts with `[ OK ]` or `[ FAIL ]` (a failing check also prints
`[ FAIL ] file:line: condition`).
`test_runner` shows what to expect under each test name, and the runner's own log lines appear in between. Two
scenarios fail **on purpose** (a job that exits 1, a program that cannot be started), so `1 failed` in the runner log
there is the expected result. Only a `[ FAIL ]` line or a non-zero exit of `make test` means a problem.
The `/bin/sh` scenarios are POSIX only; the self-child scenario also runs on Windows.

## Repository layout

```
src/platform.h    socket portability (Winsock / POSIX)
src/csv.*         account import and --generate
src/wire.*        flat fields, text/JSON rendering, form decoding
src/herd.*        in-memory state, one mutex, no socket access
src/api.*         routing and request validation (testable without sockets)
src/http.*        bounded-thread HTTP/1.1 listener, one request per connection
src/herdclient.*  shared client core: register, poll, commands (used by the runner and by domlem)
src/runner.*      optional runner: logical clients over the HTTP API
src/httpclient.*  minimal HTTP client (runner only)
src/process.*     direct program launch, no shell (runner only)
src/main.cpp      command line, signals
src/version.h     NSHTESTHERD_VERSION, the single source of the version
tests/            test_core, test_runner, integration.sh
examples/         users.csv, worker.sh (Bash worker, no Domino calls)
examples/k6/      k6 worker example: script, run.sh (hands-free end-to-end run), compose file, README
domlem/           Domino add-in worker (Notes C API): client core + Notes hooks, user registration, makefile
docs/PROTOCOL.md  HTTP API and worker contract
docs/RUNNER-PROGRAMS.md  programs the runner starts: environment, arguments, exit codes, stop
Dockerfile        static Alpine build into a scratch image (docker/, build.sh)
docker-compose.yml  coordinator + runner stack (see "Docker Compose stack")
.github/          ci.yml (build + tests), release.yml (static binaries + GHCR image)
push-release.sh   updates version.txt and pushes the vX.Y.Z tag
CHANGES.md        what changed in every version (also the text of the GitHub release)
```

## Releasing (maintainers)

The version lives in one place: `NSHTESTHERD_VERSION` in [src/version.h](src/version.h) (currently `0.9.1`). It is what
`./nshtestherd --version` prints and what the server shows in its startup line. `version.txt` is a convenience copy
(anyone can read the latest released version without parsing the header); it plays no part in the build.

Every change goes into [CHANGES.md](CHANGES.md) together with the change itself, under the heading of the coming
version (`## X.Y.Z`, the version in `src/version.h`).

| Step                 | What happens                                                                                |
| -------------------- | ------------------------------------------------------------------------------------------- |
| Push or pull request | `ci.yml`: `make`, `make test`, `tests/integration.sh` on Ubuntu                             |
| `./push-release.sh`  | Refuses to run without a `## X.Y.Z` section in `CHANGES.md`. Updates `version.txt` (commits it if changed), re-tags and pushes `vX.Y.Z`, and prints the `CHANGES.md` section as the release text |
| Publish a release    | `release.yml`: static amd64 and arm64 binaries (+ `.sha256`) and a multi-arch image on GHCR |

To release: check the `## X.Y.Z` section in `CHANGES.md`, change `NSHTESTHERD_VERSION`, commit, run `./push-release.sh`,
then publish the release for the new tag on GitHub with the text the script printed.

Only the `nshtestherd` binary is a deliverable. The test programs are built by `make test` and CI only; they are not in the
release assets or in the image.

## License

Apache License 2.0, see [LICENSE](LICENSE).
