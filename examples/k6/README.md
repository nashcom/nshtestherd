# k6 worker example

This folder shows how to write an nshtestherd worker with [k6](https://k6.io), and how to run it, also in a container.
You do not need to know k6: follow the steps below.

**What is k6?** An open-source tool for load and performance tests. A test is a small JavaScript file. k6 starts a number
of *virtual users* (VUs) and each one runs the file's `default` function in a loop. It prints a summary of all requests
(durations, failures, checks) when it ends.

**What does this example do?** [herd-worker.js](herd-worker.js) makes every virtual user an nshtestherd worker:

1. it registers and receives its own test account and `test_id`,
2. it polls the coordinator, follows the commands `run`, `pause`, `stop` and `idle`, and reports its state,
3. while it is `running` it sends one HTTP request per poll to a target URL. By default that is the coordinator's own
   `/status` endpoint, so the example needs no other system and you can watch the effect in the coordinator's metrics.

It is an example of the [worker contract](../../docs/PROTOCOL.md#worker-contract), not a load test of its own. For a real
test replace `doTest()` in the script with your requests; the account of the worker is in `me` (`me.shortname`,
`me.password`, `me.internetaddress`, ...).

## Try it hands-free

One command runs the whole example end to end. You need Docker and curl, nothing else:

```bash
examples/k6/run.sh
```

[run.sh](run.sh) builds the coordinator image from this repository (or pulls one, see `HERD_IMAGE`), starts the coordinator
and 5 k6 workers in containers, and then drives them through the complete cycle by itself:

1. waits until all workers are registered,
2. sends `run` and checks that every worker reports `running` and that the target is really being requested,
3. sends a timed `pause`, checks that every worker pauses, that the target requests really stop during the pause and start again
   after the resume,
4. sends `stop` and checks that every worker reports `done`,
5. checks the coordinator's metrics, shows the k6 summary and removes everything it started.

It prints `[ OK ]` or `[ FAIL ]` for every step and a summary at the end, and exits with 0 when everything passed (about a
minute, plus the image build on the first run). The container names start with `nshtestherd-k6-demo`, so it does not touch
a coordinator you run yourself, except that it needs its port free (`PORT=8800 examples/k6/run.sh` to change it).

| Setting         | Default             | Meaning                                                           |
| --------------- | ------------------- | ----------------------------------------------------------------- |
| `VUS`           | `5`                 | Number of k6 workers                                              |
| `USERS`         | `VUS` + 5           | Accounts generated for the coordinator                            |
| `PORT`          | `8788`              | Port of the coordinator on `127.0.0.1`                            |
| `RUN_SECONDS`   | `10`                | How long the workers stay in `run`                                |
| `PAUSE_SECONDS` | `6`                 | Length of the timed pause                                         |
| `POLL_SECONDS`  | `1`                 | Polling interval of the workers                                   |
| `HERD_IMAGE`    | built from source   | Use this image instead, e.g. `ghcr.io/nashcom/nshtestherd:latest` |
| `K6_IMAGE`      | `grafana/k6:latest` | The k6 image                                                      |

```bash
VUS=20 RUN_SECONDS=30 examples/k6/run.sh
examples/k6/run.sh --keep        # leave the coordinator running afterwards, to look around
```

The sections below explain the pieces, if you want to run them yourself.

## Files

| File                                     | Purpose                                                         |
| ---------------------------------------- | --------------------------------------------------------------- |
| [run.sh](run.sh)                         | Hands-free end-to-end run with checks (needs Docker and curl)   |
| [herd-worker.js](herd-worker.js)         | The k6 script (one virtual user = one worker)                   |
| [docker-compose.yml](docker-compose.yml) | Runs the script in the official k6 container, next to our stack |

k6 is available as an official container image, `grafana/k6`, so there is nothing to install and nothing to build: the
container simply runs our script. Writing tests as code that runs the same way on a laptop, in a container or in CI is the
modern way to do load testing, and k6 is a good first step into it.

## Option 1: all in containers (nothing to install except Docker)

From the repository root:

```bash
docker compose up -d --build                         # the coordinator, with 100 generated accounts
docker compose -f examples/k6/docker-compose.yml up  # runs the script in grafana/k6 with 5 workers; keep it open
```

k6 prints `worker 1: registered as load000001 (test_id 1)` for every worker. Now, in a second shell, control them:

```bash
H=http://127.0.0.1:8788
curl $H/status                                                          # clients_total=5
curl -X POST -d 'target=all&command=run&job=demo' $H/command            # workers start requesting the target
curl -X POST -d 'target=all&command=pause&pause_seconds=30' $H/command  # pause for 30 seconds, then resume
curl -X POST -d 'target=all&command=stop' $H/command                    # workers report done
curl $H/metrics
```

Stop the workers with Ctrl+C (or wait for `DURATION`, default 5 minutes), and the coordinator with `docker compose down`.
More workers or a different target:

```bash
VUS=20 DURATION=10m TARGET_URL=https://myserver.example/health docker compose -f examples/k6/docker-compose.yml up
```

## Option 2: coordinator on another machine, plain Docker

Feed the script to the official k6 image on standard input. Settings are environment variables:

```bash
docker run --rm -i -e HERD_URL=http://coordinator:8788 -e VUS=20 grafana/k6 run - < examples/k6/herd-worker.js
```

Coordinator on the same machine, not in the Compose stack? On Linux add `--network host` and use
`HERD_URL=http://127.0.0.1:8788`; on Docker Desktop (Windows, macOS) use `HERD_URL=http://host.docker.internal:8788`.

## Option 3: k6 installed on your machine

- **Windows:** `winget install k6 --source winget` (or `choco install k6`)
- **macOS:** `brew install k6`
- **Linux:** a package or a binary, see the [k6 install guide](https://grafana.com/docs/k6/latest/set-up/install-k6/)
- **Any system with Docker:** nothing to install, see options 1 and 2

Then, with a coordinator running (`./nshtestherd --generate 20`):

```bash
k6 run examples/k6/herd-worker.js
k6 run -e VUS=20 -e DURATION=10m -e HERD_URL=http://coordinator:8788 examples/k6/herd-worker.js
```

## Settings

The same names work as `k6 run -e NAME=value`, as `docker run -e NAME=value` and as environment of the Compose file:

| Variable       | Default                 | Meaning                                                                |
| -------------- | ----------------------- | ---------------------------------------------------------------------- |
| `HERD_URL`     | `http://127.0.0.1:8788` | Coordinator (in the Compose file: `http://herd:8788`)                  |
| `TARGET_URL`   | `$HERD_URL/status`      | URL requested once per poll while the worker is `running`              |
| `VUS`          | `5`                     | Number of workers (virtual users)                                      |
| `DURATION`     | `5m`                    | How long k6 runs                                                       |
| `POLL_SECONDS` | `2`                     | Seconds between status reports and target requests                     |
| `RUN_ID`       | random                  | Part of each `request_key`; unique per k6 instance in distributed runs |

`HERD_NETWORK` is only used by the Compose file: the Docker network of the coordinator's stack (default
`nshtestherd_default`, which is the repository folder name plus `_default`).

## What you see

- **Idle workers:** until you send `run`, the workers register and poll but request nothing. `curl $H/status` shows
  `clients_total=5` and `clients_idle=5`.
- **After `run`:** every poll the k6 summary counts one `target` request per worker. The check `target answers 200` should pass.
- **After `stop`:** workers report `done` (`clients_done=5`) and idle until k6 ends. A finished client keeps its account
  until the coordinator is restarted.
- **Summary at the end:** k6 prints request counts and durations per group: `herd_register`, `herd_status` and `target`.

## How the script maps to the protocol

| Step in the script       | API call                                                                           |
| ------------------------ | ---------------------------------------------------------------------------------- |
| First iteration of a VU  | `POST /register` with a `request_key` (a retry returns the same account)           |
| Every iteration          | `POST /status` with `state` and `ack_command_id`; the reply is the current command |
| New `command_id` seen    | Apply it once: `run`, `pause` (own timer), `idle` or `stop` (`stopping`, `done`)   |
| `404` on a status report | The allocation is lost: the worker stops its load, no re-register                  |

The script asks for JSON replies (`Accept: application/json`), so `res.json()` gives the flat fields directly. k6 sends an
object body as a form (`application/x-www-form-urlencoded`), which is what the API expects.

## When k6 ends, and distributed runs

- **`stop` or k6 ending.** A `stop` command makes a worker report `done`. When k6 just ends (`DURATION` reached, Ctrl+C) the
  workers disappear without a final report: the coordinator keeps their last reported state (`running` or `paused`) and
  their accounts stay reserved until the coordinator is restarted. There is no automatic recovery, by design. Send `stop`
  first if you want clean `done` states.
- **Restarting the coordinator.** Stop the workers first. A restarted coordinator is a fresh herd and may give the same
  `test_id` to another client, so a worker cannot tell a restart from its own state. If a status report ever returns `404`
  the script treats the allocation as lost and stops that worker's load; it never registers again by itself.
- **Several k6 instances** (for example on different machines, all against one coordinator): give every instance its own
  `RUN_ID`, for example `-e RUN_ID=$(hostname)`. Virtual user numbers start at 1 in every instance, so two instances with
  the same `RUN_ID` would use the same registration keys and share accounts. Without `RUN_ID` a random one is used per
  instance, which is fine for a single run but makes the keys hard to trace.

## Troubleshooting

| Symptom                               | Cause / fix                                                              |
| ------------------------------------- | ------------------------------------------------------------------------ |
| `connection refused` / `no such host` | Wrong `HERD_URL`, coordinator down, or k6 is in another Docker network   |
| Compose: network not found            | Start the repository's stack first, or set `HERD_NETWORK` (see Settings) |
| 409 `pool_exhausted`                  | More workers than free accounts: lower `VUS` or restart the coordinator  |
| 409 `no_accounts_loaded`              | The coordinator has no accounts: start it with `--generate N` or `--csv` |
| Workers register but do nothing       | By design: send the `run` command                                        |
| k6 ends after a few minutes           | That is `DURATION` (default 5 minutes): raise it                         |
