# Changes

Newest version first. Every change is noted here with the change itself; `push-release.sh` refuses to tag a version
that has no section here, and prints the section as the text for the GitHub release.

## 0.9.1

**New tool: `nshtestusers`.** Makes the account CSV for `--csv` (and for your registration tool): numbered users or
users with random unique names. It has its own section in the README ("Making the user CSV") and lives in
`tools/nshtestusers/`; it uses `src/csv.cpp`, so the file it writes is read by the same parser as the coordinator's.

- Numbered users with the options of `--generate` (`--generate`, `--prefix`, `--password`, `--domain`) plus `--start`
  to continue an earlier list; with the defaults the list is identical to what `--generate` hands out.
- Random unique names (`--names`): 1210 first and 1270 last names (ASCII), every combination at most once, short
  names and addresses checked for duplicates; `--seed` for the same list again, `--exclude <file>` to skip the names
  of an earlier list.
- Passwords: one for everybody, or `--random-passwords` (a different one per user, from the operating system's
  generator, length 8 to 128).
- `--output` writes the file readable for the owner only and does not replace an existing file without `--force`;
  `--check <file>` validates a user CSV.
- `make` builds it with `nshtestherd`, `make test` runs the new `test_users`, and the container image has it as
  `/nshtestusers` (`docker run --rm --entrypoint /nshtestusers ghcr.io/nashcom/nshtestherd --generate 50 > users.csv`).

**Renamed environment variables.** The variables the runner sets for its programs are now `NSHTEST_*` instead of
`NSH_*`: `NSHTEST_TEST_ID`, `NSHTEST_FIRSTNAME`, `NSHTEST_LASTNAME`, `NSHTEST_SHORTNAME`, `NSHTEST_INTERNETADDRESS`,
`NSHTEST_PASSWORD`, `NSHTEST_JOB`, `NSHTEST_COMMAND_ID`, `NSHTEST_SERVER`. The old names are still set and
`{NSH_...}` placeholders still work, so existing programs keep running; both are deprecated and will be removed in a
later release.

Coordinator and protocol:

- Optional token: with `NSHTEST_TOKEN` set (16-256 characters, environment only), every call except `GET /health`
  needs `Authorization: Bearer <token>`, otherwise 401 `unauthorized`. Without it nothing changes.
- Job parameters: `run` takes an optional `params` field (`name=value&name`, up to 1024 characters). The worker gets
  them with the instruction (`params` in the `/status`, `/register` and `/client` replies); runner programs get
  `NSHTEST_PARAMS`.
- Job results: workers report how a job ended (`last_job_id`, `last_job`, `last_job_result` = `ok`, `failed` or
  `stopped`), counted once per job. `/client` shows the last job of a client, `/status` has `jobs_ok`, `jobs_failed`
  and `jobs_stopped`, `/metrics` has `nshtestherd_jobs_ended_total{result="..."}`.
- Status messages may be up to 2048 bytes (was 512).
- Worker options: `--worker-options <name=value&name>` is returned with every registration (`worker_options`). The
  coordinator does not interpret it; each worker takes what it knows (domlem: `switch`) and ignores the rest.
- Without `--csv` or `--generate` the coordinator generates 100 accounts, so it works without any option.
  `--generate 0` starts with an empty pool that waits for `POST /load` (what an option-less start did before).

Workers and runner:

- A job that fails no longer ends the worker: it is reported as `failed` and the worker is `idle` again, waiting for
  the next command. Only `stop` (or the worker's own shutdown) ends a worker; `error` is left for a worker that cannot
  work at all. For the runner a failing program is a failed job, not a failed client (exit code 0).
- A newer `idle`, `run` or `stop` ends a job that is still running; it is reported as `stopped`.
- A job result is kept until the coordinator has accepted a report that carried it: when the next job ends first (for
  example `stopped` by a `run` that then cannot start) after a failed report, the client delivers the old result before
  it replaces it, so no job is lost from the counts.
- The client core cuts status messages to the coordinator's limit instead of being refused on every poll.
- The protocol logic of the runner is now a shared client core (`src/herdclient.*`) that other workers can use.
- `examples/worker.sh` and the k6 example report job results and send the token.

New: **domlem**, a Domino add-in worker (Notes C API) built on the shared client core, with the jobs `dbopen`,
`agent` / `agent:<name>`, `mail` and `mailtest`, and optional user registration and identity switch (`-switch`).
No database stays open between two operations: each one opens what it needs and closes it in reverse order (notes before
their database, the agent before its database), and every database is closed with `NSFDbCloseSession` unless
`-closesession 0` is given. See [domlem/README.md](domlem/README.md).

Build: objects are rebuilt when the compiler, the C library or the flags change (`.build-id`) and when the Makefile
changes; objects are built with `-fPIC`.

Tests: `tests/testrun.sh [seconds]` (default 60) runs a coordinator with two runners under random commands, stops
them, and checks that every job result the runners logged was counted exactly once (`/status` and `/metrics`).

## 0.9.0

First release: coordinator (account pool, sequential test ids, commands, Prometheus metrics), optional runner,
`--generate`, Docker image and Compose stack, k6 and Bash worker examples.
