# nshtestherd protocol (v1)

Everything a worker or an operator can do goes through this HTTP API. All examples use `curl`
against `http://127.0.0.1:8788` (the default `--bind` / `--port`).

Contents: [Quick walkthrough](#quick-walkthrough) | [Formats](#formats) | [Endpoints](#endpoints) |
[Errors](#errors) | [Worker contract](#worker-contract)

## Quick walkthrough

A complete session with two workers. Start the server with three generated accounts:

```bash
./nshtestherd --generate 3
```

**1. Is it up?**

```bash
curl http://127.0.0.1:8788/health
```

```ini
status=ok
```

**2. Two workers register.** Each worker invents a `request_key` once and reuses it on every retry.

```bash
curl -X POST -d request_key=worker-a http://127.0.0.1:8788/register
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
pause_seconds=0
```

```bash
curl -X POST -d request_key=worker-b http://127.0.0.1:8788/register
```

The second worker gets `test_id=2` and `shortname=load000002`. Registering `worker-a` again returns the same
account (HTTP 200 instead of 201):

```bash
curl -i -X POST -d request_key=worker-a http://127.0.0.1:8788/register
```

```ini
HTTP/1.1 200 OK
...
test_id=1
shortname=load000001
...
```

**3. How is the herd doing?**

```bash
curl http://127.0.0.1:8788/status
```

```ini
users_total=3
users_available=1
users_allocated=2
clients_total=2
clients_registered=2
clients_idle=0
clients_running=0
clients_paused=0
clients_stopping=0
clients_done=0
clients_error=0
uptime_seconds=12
```

**4. Tell everyone to run a job.**

```bash
curl -X POST -d 'target=all&command=run&job=mail-read' http://127.0.0.1:8788/command
```

```ini
target=all
command=run
updated=2
```

**5. A worker polls, applies the command and acknowledges it** (`ack_command_id` = the id it applied):

```bash
curl -X POST -d 'test_id=1&state=running&ack_command_id=1' http://127.0.0.1:8788/status
```

```ini
test_id=1
command=run
command_id=1
job=mail-read
pause_seconds=0
```

**6. Inspect one client** (desired command and reported state side by side, never the password):

```bash
curl 'http://127.0.0.1:8788/client?test_id=1'
```

```ini
test_id=1
shortname=load000001
state=running
ack_command_id=1
last_contact_seconds=0
message=
command=run
command_id=1
job=mail-read
pause_seconds=0
```

**7. Pause one client for 30 seconds, and the worker acknowledges:**

```bash
curl -X POST -d 'test_id=1&command=pause&pause_seconds=30' http://127.0.0.1:8788/command
```

```ini
test_id=1
command_id=2
command=pause
updated=1
```

```bash
curl -X POST -d 'test_id=1&state=paused&ack_command_id=2' http://127.0.0.1:8788/status
```

**8. Stop everyone.** Each worker finishes its operation, reports `stopping`, then `done`:

```bash
curl -X POST -d 'target=all&command=stop' http://127.0.0.1:8788/command
curl -X POST -d 'test_id=1&state=stopping&ack_command_id=3' http://127.0.0.1:8788/status
curl -X POST -d 'test_id=1&state=done&ack_command_id=3&message=finished' http://127.0.0.1:8788/status
```

**9. Metrics for Prometheus:**

```bash
curl http://127.0.0.1:8788/metrics
```

## Formats

* **Requests** are `application/x-www-form-urlencoded` (percent escapes and `+` decoded), sent with
  `curl -d`. Exception: `POST /load` takes raw `text/csv`. Unknown, duplicate or malformed fields are
  rejected with 400. A single trailing line break in a form body is ignored.
* **Responses** are flat, lower-case-key, scalar fields only. The default is sectionless INI-style text
  (`text/plain; charset=utf-8`), one `key=value` per line. Split each line at the **first** `=`; keep
  spaces and further `=` in the value. Escapes: `\\`, `\n`, `\r`, `\t` (backslash, LF, CR, TAB).
* Send `Accept: application/json` to get the same fields as one flat JSON object (numbers unquoted).
  `/metrics` is always Prometheus text.
* Errors use `error` (machine code) and `message` fields in the selected format.
  Protocol-level failures (oversized request, bad HTTP, timeout) are always text.
* Each connection carries one request (`Connection: close`). Send `Content-Length` (curl does).

JSON works on every endpoint:

```bash
curl -X POST -H 'Accept: application/json' -d request_key=worker-c http://127.0.0.1:8788/register
```

```json
{"test_id":3,"firstname":"Load","lastname":"000003","password":"TestPassword","shortname":"load000003","internetaddress":"load000003@example.com","command":"idle","command_id":0,"job":"","pause_seconds":0}
```

| Status | Meaning                                                                |
| ------ | ---------------------------------------------------------------------- |
| 201    | New registration                                                       |
| 200    | Success / retry / read                                                 |
| 400    | Invalid field or body                                                  |
| 404    | Unknown path or `test_id`                                              |
| 405    | Method not allowed (`Allow` header set)                                |
| 409    | Pool exhausted/not loaded, reload not allowed, client already finished |
| 413    | Request too large                                                      |
| 501    | `Transfer-Encoding` (chunked requests) not supported                   |
| 503    | Connection queue full                                                  |

Add `-i` to any curl call to see the status line, for example `curl -i http://127.0.0.1:8788/health`.

## Endpoints

| Method and path         | Purpose                                            |
| ----------------------- | -------------------------------------------------- |
| `POST /load`            | Import or replace the account pool (CSV)           |
| `POST /register`        | Get an account and a `test_id`                     |
| `POST /status`          | Worker: report state, acknowledge, get the command |
| `POST /command`         | Operator: set the desired command                  |
| `GET /client?test_id=N` | Inspect one client                                 |
| `GET /status`           | Summary counts                                     |
| `GET /metrics`          | Prometheus metrics                                 |
| `GET /health`           | Liveness                                           |

### POST /load - import accounts

Body: CSV (see the README), `Content-Type: text/csv`. Replaces the pool; allowed only until the first client
registers. Not needed when the server was started with `--csv` or `--generate`.

```bash
curl -X POST -H 'Content-Type: text/csv' --data-binary @examples/users.csv http://127.0.0.1:8788/load
```

```ini
users=10
```

A bad CSV is rejected as a whole and the old pool stays:

```bash
curl -i -X POST -H 'Content-Type: text/csv' --data-binary $'A,B,pw,x1,m\nbroken\n' http://127.0.0.1:8788/load
```

```ini
HTTP/1.1 400 Bad Request
...
error=invalid_csv
message=record 2: expected 5 columns, found 1
```

After the first registration a reload returns 409 `load_not_allowed`.

### POST /register - get an account and a test_id

Optional `request_key` (1-128 printable ASCII characters, no spaces): generate it once per worker and reuse
it on every retry; the original allocation and `test_id` come back (HTTP 200). Without a key every call
allocates a new account, so callers that retry should always send a key. The service always chooses the
`test_id` (1, 2, 3, ...); failed requests do not consume a number and numbers are never reused during one run.

```bash
curl -X POST -d request_key=pod-7f3a http://127.0.0.1:8788/register
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
pause_seconds=0
```

When no account can be given:

```bash
curl -i -X POST http://127.0.0.1:8788/register
```

```ini
HTTP/1.1 409 Conflict
...
error=pool_exhausted
message=all accounts are allocated
```

The other 409 code is `no_accounts_loaded` (start with `--csv` / `--generate` or `POST /load` first).

### POST /status - report and poll

Fields: `test_id` and `state` (required), `ack_command_id`, `message` (optional, max 512 bytes). `state` is
one of `idle running paused stopping done error`. The reply is the current instruction (`test_id`,
`command`, `command_id`, `job`, `pause_seconds`). Every call updates the last-contact time.

`ack_command_id` means "applied", not "completed". It cannot exceed the issued `command_id` (otherwise 400),
and it **never decreases**: a delayed report carrying an older id leaves the stored acknowledgement
unchanged. The reported `state` and `message` are last-writer-wins, so a worker should send its reports in
order. After a client reports `done` or `error` it stays finished: later reports only refresh contact time.

A client that has not been given a command yet has `command_id=0`, so its first report acknowledges 0:

```bash
curl -X POST -d 'test_id=1&state=idle&ack_command_id=0&message=ready' http://127.0.0.1:8788/status
```

```ini
test_id=1
command=idle
command_id=0
job=
pause_seconds=0
```

After `POST /command` has issued command 2 to this client, the worker applies it and acknowledges:

```bash
curl -X POST -d 'test_id=1&state=running&ack_command_id=2&message=ok' http://127.0.0.1:8788/status
```

Acknowledging a command that was never issued is rejected:

```bash
curl -i -X POST -d 'test_id=1&state=idle&ack_command_id=9' http://127.0.0.1:8788/status
```

```ini
HTTP/1.1 400 Bad Request
...
error=invalid_ack_command_id
message=ack_command_id is newer than the issued command_id
```

An unknown `test_id` returns 404 `unknown_test_id`.

### POST /command - set an instruction

Either `test_id=N` (one client) or `target=all` (every client that exists now and has not finished), plus `command`:

| command | Fields                     | Worker behaviour                                                   |
| ------- | -------------------------- | ------------------------------------------------------------------ |
| `idle`  | none                       | Stay alive, no load, keep polling                                  |
| `run`   | `job`                      | Start or continue the named workload (a name, not a shell command) |
| `pause` | `pause_seconds` 1-31536000 | Pause that long, keep polling, then resume the previous state      |
| `stop`  | none                       | Finish the current operation, report `done`, exit                  |

`job` is 1-128 characters of `A-Z a-z 0-9 . _ : / -`.

```bash
curl -X POST -d 'target=all&command=run&job=mail-read'     http://127.0.0.1:8788/command
curl -X POST -d 'test_id=1&command=pause&pause_seconds=30'  http://127.0.0.1:8788/command
curl -X POST -d 'target=all&command=stop'                   http://127.0.0.1:8788/command
```

Reply for a single client: `test_id`, `command_id` (the new id), `command`, `updated=1`.
Reply for `target=all`: `target=all`, `command`, `updated` (number of clients that were affected).

Every accepted update gives each affected client a new `command_id` (repeating a submission is intentionally a
new id). Clients registering later start idle. A command to a finished client returns 409 `client_finished`,
and `target=all` skips finished clients. Invalid combinations are rejected:

```bash
curl -i -X POST -d 'test_id=1&command=run' http://127.0.0.1:8788/command
```

```ini
HTTP/1.1 400 Bad Request
...
error=invalid_request
message=run requires job (1-128 characters of A-Z a-z 0-9 . _ : / -)
```

### GET /client - inspect one client

```bash
curl 'http://127.0.0.1:8788/client?test_id=1'
```

Fields: `test_id`, `shortname`, `state` (as reported by the worker), `ack_command_id`, `last_contact_seconds`,
`message`, and the desired instruction (`command`, `command_id`, `job`, `pause_seconds`). No password.
A client whose `ack_command_id` is lower than its `command_id` has not applied the latest command yet.

### GET /status - summary

```bash
curl http://127.0.0.1:8788/status
```

`users_total users_available users_allocated clients_total`, one `clients_<state>` count per state and
`uptime_seconds`. No credentials. (A full example is in the walkthrough.)

### GET /metrics - Prometheus

```bash
curl http://127.0.0.1:8788/metrics
```

```
# HELP nshtestherd_users_total Accounts in the loaded pool.
# TYPE nshtestherd_users_total gauge
nshtestherd_users_total 3
...
# HELP nshtestherd_clients_by_state Registered clients by last reported state.
# TYPE nshtestherd_clients_by_state gauge
nshtestherd_clients_by_state{state="registered"} 0
nshtestherd_clients_by_state{state="idle"} 0
nshtestherd_clients_by_state{state="running"} 1
nshtestherd_clients_by_state{state="paused"} 1
nshtestherd_clients_by_state{state="stopping"} 0
nshtestherd_clients_by_state{state="done"} 0
nshtestherd_clients_by_state{state="error"} 0
...
```

Prefix `nshtestherd_`, content type `text/plain; version=0.0.4`.

| Metric                          | Type    | Meaning                                                   |
| ------------------------------- | ------- | --------------------------------------------------------- |
| `users_total`                   | gauge   | Accounts in the pool                                      |
| `users_available`               | gauge   | Accounts not yet allocated                                |
| `users_allocated`               | gauge   | Accounts allocated to clients                             |
| `clients_total`                 | gauge   | Registered clients                                        |
| `uptime_seconds`                | gauge   | Seconds since the coordinator started                     |
| `clients_by_state{state="..."}` | gauge   | Clients by last reported state (all seven states, always) |
| `registrations_total`           | counter | New allocations only                                      |
| `allocation_failures_total`     | counter | Registrations refused, no account left                    |
| `status_reports_total`          | counter | Accepted status reports                                   |
| `command_updates_total`         | counter | Command updates, per affected client                      |
| `csv_loads_total`               | counter | Successful account imports                                |
| `csv_load_failures_total`       | counter | Rejected account imports                                  |

State label values: `registered`, `idle`, `running`, `paused`, `stopping`, `done`, `error`. No test ids, names,
addresses, keys, messages or job names appear as labels.

### GET /health

```bash
curl http://127.0.0.1:8788/health
```

```ini
status=ok
```

Cheap liveness check.

## Errors

Every error has the same two fields in the selected format:

```bash
curl -i http://127.0.0.1:8788/nope
```

```ini
HTTP/1.1 404 Not Found
...
error=not_found
message=unknown path
```

```bash
curl -i http://127.0.0.1:8788/register
```

```ini
HTTP/1.1 405 Method Not Allowed
Allow: POST
...
error=method_not_allowed
message=method not allowed for this path
```

| `error`                  | Status | Cause                                                                |
| ------------------------ | ------ | -------------------------------------------------------------------- |
| `invalid_request`        | 400    | Unknown, duplicate or malformed field; bad value; wrong content type |
| `invalid_csv`            | 400    | CSV rejected, `message` has the record number                        |
| `invalid_ack_command_id` | 400    | `ack_command_id` is newer than the issued `command_id`               |
| `not_found`              | 404    | Unknown path                                                         |
| `unknown_test_id`        | 404    | No client with that `test_id`                                        |
| `method_not_allowed`     | 405    | Wrong method for the path                                            |
| `no_accounts_loaded`     | 409    | No pool yet                                                          |
| `pool_exhausted`         | 409    | Every account is allocated                                           |
| `load_not_allowed`       | 409    | A client has already registered                                      |
| `client_finished`        | 409    | Command sent to a client that is `done` or `error`                   |

The same set in JSON: `curl -H 'Accept: application/json' -i http://127.0.0.1:8788/nope` returns
`{"error":"not_found","message":"unknown path"}`.

## Worker contract

1. Generate a `request_key` once; `POST /register` (retry with the same key on failure).
2. Prepare identity from the returned account (a Domino worker builds its Notes name and derives its
   organization from its own admin identity).
3. Loop: `POST /status` with the observed `state` and the last **applied** `command_id`.
4. Apply each `command_id` exactly once. If the id equals the last applied one, do nothing - in particular do
   not restart the job or reset a pause timer.
5. Pause: start a monotonic timer when the instruction is applied; do not re-arm it on later polls. Switch
   jobs and pause at safe operation boundaries.
6. Unknown `job`: report `state=error` with a message. Done: report `done` and exit.

Coordinator-desired instruction and worker-reported state are independent: after a timed pause expires a
worker reports `running` while the last desired instruction is still the already-acknowledged `pause`.

**Commands are the latest desired instruction, not a queue.** If several commands are issued between two
polls, the worker sees only the newest one (its `command_id` may have jumped by more than one). Intermediate
commands are never delivered, and the worker acknowledges the newest id it applied.

**Pause expiry resumes state, it does not replay commands.** When a timed pause ends the worker returns to the
state it was in before the pause. A `run` that already completed is not launched again: for an external program
that finished before the pause was applied, the previous state is `idle`, so resuming leaves it idle and the
acknowledged `run` command id is not executed a second time. Only a new `command_id` starts new work.

`examples/worker.sh` implements this contract in Bash. The built-in runner (`nshtestherd --runner`, see the
README) is a worker host that follows the same contract through the public API; it is optional and uses no
private interface. For external programs it applies each command id once, between child executions, and
reports a child's result as `idle` (exit 0) or `error` (anything else). The runner owns status reporting for its
clients: a child program must not post `/status` under the same `test_id`.
