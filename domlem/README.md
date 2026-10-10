# domlem - the Domino Performance Test Lemming

**domlem** is a Domino add-in task that generates real load on a Domino server. It works as an
[nshtestherd](../README.md) worker: it registers with the nshtestherd coordinator, receives a test account, and then only
follows the commands the coordinator sends (`run <job>`, `pause`, `idle`, `stop`). Like a lemming it does what it is
told, and you start many of them.

The work itself is real Notes API work, done in the task's own process:

| Job      | What one step does                                                                                  |
| -------- | --------------------------------------------------------------------------------------------------- |
| `dbopen` | Opens a database as the current user, reads the access level, closes it again                       |
| `agent`  | Runs a Domino agent of a database; `agent:<name>` picks the agent per run command                   |
| `mail`   | Crafts a complete mail (text, attachments with icons) and writes it into the mail.box of the server |
| `mailtest` | The full mail test: like `mail`, with random subject, 1-3 random test users as recipients and a sent copy |

Optionally every lemming works as its own user: it registers the user (with mail file) when it does not exist yet, takes
the ID from the ID vault and switches to it (`-switch`).

## Scale up first, then work

Start as many lemmings as you like, before or after the coordinator, before or after the accounts are loaded. A lemming
waits until it gets an account, and it says so: the status line of the task (`show tasks`) reads
`Waiting for an account: ...` with the reason (`coordinator unreachable`, `no account pool loaded yet`, `no free account
left`), and the log has one line when the reason changes. Waiting is the default; `-nowait` makes a lemming end
instead when no account is free. More lemmings than accounts is fine: the extra ones keep
waiting (the coordinator's metric `nshtestherd_allocation_failures_total` counts their attempts). Once a lemming has an
account it sets up (with `-switch`: user, mail file, ID, identity switch) and then reports `idle`. On the coordinator a
lemming shows as `registered` while it is setting up and as `idle` when it is ready for work, so a script can wait for
`clients_idle` to reach the number of lemmings before it sends the first `run`.

## Quick start

You need a Domino server with the Notes C API build environment (see [Build](#build)) and a running coordinator.

**1. Start the coordinator** (any machine that the server can reach, see the main README), with some test accounts:

```bash
nshtestherd --generate 20
```

**2. Build and install domlem on the Domino server**:

```bash
cd domlem
make
make install
```

**3. Start one lemming.** Without any option it talks to a coordinator on the same machine
(`http://127.0.0.1:8788`) and works against the server it runs on, with the identity of the server:

```
load domlem
```

or, with a coordinator on another machine:

```
load domlem -coordinator http://coordinator:8788
```

or set the coordinator once for every lemming of the server, in the server's environment or in `notes.ini`, and `load
domlem` needs no option again (`-coordinator` still overrides it):

```
set config DOMLEM_COORDINATOR=http://coordinator:8788
load domlem
```

`show tasks` shows `domlem` with the status line `Idle: waiting for work`. The status line always shows the state and the
message the coordinator gets (`Running: mail #12 submitted ...`, `Idle: job mail failed: ...`, `Paused: paused 30s`).
The coordinator shows the new client:

```bash
H=http://127.0.0.1:8788
curl $H/status
```

**4. Tell it what to do.** Every command goes to the coordinator, not to the lemming:

```bash
curl -X POST -d 'target=all&command=run&job=dbopen' $H/command     # start the job
curl "$H/client?test_id=1"                                         # what it did: "dbopen #12 ... access level 6"
curl -X POST -d 'target=all&command=pause&pause_seconds=30' $H/command
curl -X POST -d 'target=all&command=idle' $H/command              # the job ends ("stopped"), the lemming waits
curl -X POST -d 'target=all&command=stop' $H/command              # the lemming reports done and ends
```

A job ends when a newer `idle`, `run` or `stop` replaces it (result `stopped`) or when an operation fails (result
`failed`, with the reason). Either way the lemming goes back to `idle` and waits for the next command; only `stop` or a
server shutdown ends the task. Each client's last job and its result are in `curl "$H/client?test_id=1"`
(`last_job`, `last_job_result`, `last_job_message`), the totals in `curl $H/status` (`jobs_ok`, `jobs_failed`,
`jobs_stopped`).

That is all for a first look. The `dbopen` job opens `names.nsf` (read only: open, access level, close), so it works on
every server without preparation; `-dbopen` names another database.

## Steering the lemmings with curl

The lemmings are steered through the coordinator, never directly. `curl` is the operator's tool for that:

```
your shell (any machine)              nshtestherd (coordinator)              domlem processes (Domino server)
------------------------              -------------------------              --------------------------------
curl POST /command  ------------->    keeps the latest command per client
                                                                  <-------   poll POST /status every -poll seconds
                                      reply: command=run job=mailtest  --->  apply it, one operation per poll
curl GET /status, /client  ------>    states, last job results
```

- **Where curl runs:** on any machine that reaches the coordinator's port: your workstation, the coordinator host or the
  Domino server. Set `H` to the coordinator as seen from there. The lemmings use their own `-coordinator` URL, which can
  be a different name for the same coordinator.
- **The lemmings need no curl:** domlem has the HTTP client built in (the shared core in `../src/httpclient.cpp`). They
  connect to the coordinator; nothing connects to the Domino server, so it needs no open port for this.
- Any HTTP client works instead of curl: a browser for the `GET` calls (`http://coordinator:8788/status`), PowerShell
  `Invoke-RestMethod`, a script.

```bash
H=http://coordinator:8788
```

When the coordinator has a token, add `-H "Authorization: Bearer $NSHTEST_TOKEN"` to every call below (and give the
lemmings the same value as `NSHTEST_TOKEN`).

| Goal                                     | Command                                                                     |
| ---------------------------------------- | --------------------------------------------------------------------------- |
| How many lemmings are there, and idle?   | `curl $H/status` (`clients_total`, `clients_idle`)                          |
| Open a database on every lemming         | `curl -X POST -d 'target=all&command=run&job=dbopen' $H/command`            |
| Run the default agent (`-agent`)         | `curl -X POST -d 'target=all&command=run&job=agent' $H/command`             |
| Run a named agent of `-db`               | `curl -X POST -d 'target=all&command=run&job=agent:mailread' $H/command`    |
| Predictable mail (to `-mailto`/sender)   | `curl -X POST -d 'target=all&command=run&job=mail' $H/command`              |
| The full random mail test                | `curl -X POST -d 'target=all&command=run&job=mailtest' $H/command`          |
| Only lemming 3                           | `curl -X POST -d 'test_id=3&command=run&job=mailtest' $H/command`           |
| Bigger mails for this run only           | `curl -X POST -d 'target=all&command=run&job=mailtest' --data-urlencode 'params=mailsize=50-100' $H/command` |
| What lemming 3 does and its last job     | `curl "$H/client?test_id=3"`                                                |
| Job results of all lemmings              | `curl -s $H/status \| grep '^jobs_'`                                        |
| Pause all for 30 seconds                 | `curl -X POST -d 'target=all&command=pause&pause_seconds=30' $H/command`    |
| End the jobs, keep the lemmings          | `curl -X POST -d 'target=all&command=idle' $H/command`                      |
| End the lemmings (tasks terminate)       | `curl -X POST -d 'target=all&command=stop' $H/command`                      |
| Prometheus metrics                       | `curl $H/metrics`                                                           |

A command reaches a lemming with its next poll (`-poll`, default 2 seconds). `"$H/client?test_id=3"` needs the quotes
because of the `?`. Add `-i` to see the HTTP status, `-H 'Accept: application/json'` for JSON. The whole API is in
[docs/PROTOCOL.md](../docs/PROTOCOL.md).

What `GET /client` shows for a lemming in the middle of a `mailtest` job:

```ini
test_id=3
shortname=load000003
state=running
ack_command_id=4
last_contact_seconds=1
message=mail #17 submitted to 3 recipients with sent copy, 8200 bytes text, 2 attachments with 410000 bytes
command=run
command_id=4
job=mailtest
params=mailsize=5-10
pause_seconds=0
last_job_id=2
last_job=agent:mailread
last_job_result=stopped
last_job_message=job agent:mailread stopped
```

## Recipes

Each recipe is one command line for the lemming (`load domlem ...`) and one for the coordinator.

**Smoke test of the connection** - nothing to prepare (`dbopen` opens `names.nsf`, read only):

```
load domlem
curl -X POST -d 'target=all&command=run&job=dbopen' $H/command
```

**Mail load to a real test user** - every mail goes to that user (the server identity sends it):

```
load domlem -mailto "CN=Test User/O=Org"
curl -X POST -d 'target=all&command=run&job=mail' $H/command
```

**Mail load where every lemming is its own user** - mail goes to the lemming's own mail file. The users and their mail
files are created when missing, certified by the Domino CA of the server's own organization (`CN=srv/O=Org` gives
`/O=Org`); `-ca` names another Domino CA, `-certid` a certifier ID file:

```
load domlem -switch
curl -X POST -d 'target=all&command=run&job=mail' $H/command
```

**The full mail test** - every lemming is its own user and sends mail with a random subject, random body text and random
attachments to 1-3 random users of the test pool, and keeps a sent copy in its own mail file. No mail options needed:

```
load domlem -switch
curl -X POST -d 'target=all&command=run&job=mailtest' $H/command
```

**Mail load between the test users** - every mail goes to 2 random test users. Without `-mailfilter` the recipients
come from the test pool of the lemming's account (see [Random recipients](#random-recipients)); a filter of your own
picks other people:

```
load domlem -switch -mailrandom 2
load domlem -switch -mailrandom 2 -mailfilter 'Form="Person" & @Contains(ShortName; "test")'
```

**Bigger mails** - bodies of 20-50 KB and 0-5 attachments of 100-1000 KB:

```
load domlem -mailto "CN=Test User/O=Org" -mailsize 20-50 -mailattach 0-5 -mailattachsize 100-1000
```

**Agent load** - runs the agent `TestAgent` of the test database `nshtestherd.nsf` (create the database and the agent in
Designer, see [The agent job](#the-agent-job)):

```
load domlem
curl -X POST -d 'target=all&command=run&job=agent' $H/command             # -agent (default TestAgent)
curl -X POST -d 'target=all&command=run&job=agent:mailread' $H/command    # the agent mailread of the same database
```

**Many lemmings** - one process is one client with one identity, so start the task many times. A shell loop on the server
is the safe way (it does not depend on the console accepting the same task name repeatedly):

```bash
for i in $(seq 1 20)
do
  /opt/hcl/domino/notes/latest/linux/domlem -switch &
done
```

Every process registers itself with a unique request key and receives its own account and `test_id`. Running the binary
directly needs the Domino environment of the server user, as for any add-in.

## What a default mail looks like

You do not have to set anything to get realistic mail. The two mail jobs share the code and differ only in their
defaults; every option overrides them for both:

| Property            | `mail`                              | `mailtest`                            | Option                                     |
| ------------------- | ----------------------------------- | ------------------------------------- | ------------------------------------------ |
| Recipient           | The sender itself                   | 1-3 random users of the test pool     | `-mailto`, `-mailrandom`, `-mailfilter`    |
| Subject             | `domlem load test` + tag            | random words or sentence + tag        | `-mailsubject random\|fixed`               |
| Sent copy           | no                                  | in the sender's mail file, if it has one | `-mailsentcopy yes\|no\|auto`           |
| Body text           | 1-20 KB of lorem ipsum              | the same                              | `-mailsize`, `-mailtext` (lorem or funny)  |
| Attachments         | 0-2 per mail, 10-200 KB             | the same                              | `-mailattach`, `-mailattachsize`           |
| Attachment content  | Random bytes (they do not compress) | the same                              | `-mailattachtype text` for compressible text |
| Marked as generated | Yes (`Auto-submitted`)              | the same                              | (fixed)                                    |

The **tag** at the end of every subject tells which lemming sent which mail: `[domlem t<test_id> <short name> #<n>]`, for
example `Quis nostrud exercitation [domlem t3 load000003 #17]`. The attachment names carry the test id as well:
`attachment_t3_17_1.bin`. The numbers start at 1 with every job.

Every mail takes its own size, attachment count, attachment sizes, number of random recipients and subject out of the
ranges, evenly distributed. The values are made from the mail's number, so the same message number has the same content
on every run (the random recipients are drawn anew). The job message shows what the mail contained, for example
`mail #17 submitted to 3 recipients with sent copy, 8200 bytes text, 2 attachments with 410000 bytes`.

**Sent copy:** the message is also saved in the sender's mail file, as a Notes client does with "save sent copy". The
mail file comes from the sender's person document (`MailServer`, `MailFile`). `auto` (the `mailtest` default) skips the
copy when the sender has no mail file, for example the server identity without `-switch`, and logs that once per job;
`yes` makes that an error. The copy is a note of its own, built from the same values, written before the mail goes into
the `mail.box`.

The mail is a complete Notes memo: `Form`, `From`, `SendTo` (and `CopyTo`, `BlindCopyTo` when used), `Recipients`,
`Subject`, `Auto-submitted`, `PostedDate`, a rich text `Body` and one `$FILE` item per attachment. The body also holds an
attachment hotspot (icon and file name) for every attachment, as a Notes client shows them.

The mail is written straight into the `mail.box` of the server (`-server`, default: the server the task runs on). The
router picks it up from there, so every mail exercises the whole routing path. A server with several mail boxes hands out
one of them for every open of `mail.box`.

**Without `-switch`** the sender is the server itself. Mail to "yourself" then goes to the server's name, which fails
unless the server has a mail file. For a test without `-switch` use `-mailto` or `-mailrandom`.

## The two identity modes

| Mode              | Option    | What it does                                                         |
| ----------------- | --------- | -------------------------------------------------------------------- |
| Keep the identity | (default) | Works as the identity the process runs under, normally the server ID |
| Account identity  | `-switch` | Works as the user of the account that the coordinator allocated      |

Where the mode comes from, the first one that is set wins:

| Order | Where                                        | Example                                                          |
| ----- | -------------------------------------------- | ---------------------------------------------------------------- |
| 1     | Command line of the lemming                  | `load domlem -switch`, or `load domlem switch=0` to keep the identity |
| 2     | Server environment, else `notes.ini`         | `set config DOMLEM_SWITCH=1` (or `=0`)                           |
| 3     | The coordinator's worker options             | `nshtestherd --worker-options switch`                            |
| 4     | Default                                      | off: keep the identity                                           |

So the coordinator can turn the switch on for a whole herd of lemmings, but the Domino server has the last word: a switch
set on the server (1 or 2, also `=0`) is not changed by the coordinator; the log says so when the coordinator asks for
something else. Worker options are the same for every worker of the herd; other kinds of workers ignore `switch`.

In `-switch` mode, once per lemming, right after the coordinator has allocated an account:

1. **Lookup:** the user is searched in the Domino Directory by short name, internet address and full name.
2. **Registration:** only if the user does not exist, it is registered with the certifier: ID, directory entry, internet
   password and the mail file (`mail/<shortname>.nsf` on the server) in one step. The certifier is `-certid` (with
   `DOMLEM_CERTPW`), else `-ca`, else the Domino CA of the server's own organization (`CN=srv/O=Org` gives `/O=Org`).
3. **ID:** the ID is downloaded from the ID vault into a private file in the data directory (`SECidfGet`). For a user
   registered just now this is retried for about one minute, until the ID has reached the vault.
4. **Switch:** the process switches to that ID (`SECKFMSwitchToIDFile`, the process-wide identity). The ID file is
   removed when the process ends.

Existing users are reused as they are: nothing is registered and no ID or password is changed. Registering needs a
certifier:

| Option            | Default        | Meaning                                                                        |
| ----------------- | -------------- | ------------------------------------------------------------------------------ |
| `-ca <name>`      | the server's organization | Domino CA that certifies the new users (used without certifier ID + password) |
| `-certid <file>`  | none           | Certifier ID file, used directly when `DOMLEM_CERTPW` holds its password       |
| `-template <ntf>` | server default | Mail template for the new mail files, for example `mail14.ntf`                 |
| `-maildir <dir>`  | `mail`         | Directory of the new mail files (`<dir>/<shortname>.nsf`)                      |
| `-policy <name>`  | none           | Explicit policy for new users, for example the one that puts the ID in a vault |

The certifier password is deliberately not an option: it would show up in the process list and in `show tasks`. Set
`DOMLEM_CERTPW` instead (see [Settings from the server](#settings-from-the-server-token-certifier-password-defaults)).

## The agent job

`run job=agent` runs one agent of `-db` (default `nshtestherd.nsf`) per step, as the current identity. The agent is opened
once per job; every run has its own run context with the time limit.

`-db` must be a test database: the agent job refuses every system database, because an agent runs unrestricted and
could write anywhere in it. The list is Domino's own, `dominosystemdbs.ind` in the data directory (a built-in list of the
main system databases when the file is missing), plus every mail box (`mail.box`, `mailN.box`) and the directory of
`-mailnab`. The check uses the Domino logical name of the opened database (`NSFDbPathGet`), so a physical path, a
directory link or another spelling of a system database is caught as well. The job then fails with "does not run agents
of the system database"; the lemming stays.

`run job=agent:<name>` runs the agent `<name>` instead of `-agent`, so one test database can hold several workloads (mail
read, document updates, views, full text search, ...) and the coordinator picks one per run command. The name or an alias
of the agent works; it has to fit the job name rules (`A-Z a-z 0-9 . _ : / -`, no blanks), so give agents with blanks
in their name an alias, for example `Mail Read | mailread`. The database is always `-db` from the command line: the
coordinator can choose which agent runs, not where.

The identity is set once, before the first command (with `-switch` the account's user): every agent runs as that user.

- Create the agent in Designer in `nshtestherd.nsf`: name `TestAgent` (or give another name with `-agent`), any trigger
  that allows to run it manually or in the background, no user interface.
- Sign it with an ID that may run agents on the server.
- The agent runs unrestricted: the signer's agent privileges are not checked (no `AGENT_SECURITY_ON`), only the access
  of the current identity to the database counts.
- `-agenttimeout` (default 600 seconds, 0: none) limits one run.

## Options

| Option                  | Default                 | Meaning                                                       |
| ----------------------- | ----------------------- | ------------------------------------------------------------- |
| `-coordinator <url>`    | `DOMLEM_COORDINATOR`, else `http://127.0.0.1:8788` | nshtestherd coordinator               |
| `-server <name>`        | this server             | Domino server to work against (databases, ID vault, mail.box) |
| `-switch`               | `DOMLEM_SWITCH`, else the coordinator's worker options, else off | Work as the account's own user (see the two identity modes); `switch=0` turns it off |
| `-dbopen <path>`        | `names.nsf`             | Database the `dbopen` job opens (read only)                   |
| `-closesession <0\|1>`  | `1`                     | Every database is closed with `NSFDbCloseSession` (the session goes with it, the next open connects anew) or `NSFDbClose` (see "How it works") |
| `-db <path>`            | `nshtestherd.nsf`       | Test database with the agents of the `agent` job (never a system database) |
| `-agent <name>`         | `TestAgent`             | Agent of `-db` run by the `agent` job (`agent:<name>` overrides) |
| `-agenttimeout <sec>`   | `600`                   | Execution limit of one agent run in seconds, 0: none          |
| `-mailto <names>`       | `mail`: the sender      | Recipients of the mail jobs, comma separated                  |
| `-mailrandom <count>`   | `mail` 0, `mailtest` 1-3 | Random recipients per mail: a number or a range (0-50)       |
| `-mailfilter <formula>` | the account's test pool | Selection formula of the people to pick from                  |
| `-mailsubject <s>`      | `mail` fixed, `mailtest` random | Subject: `random` or `fixed`, always with the tag     |
| `-mailsentcopy <s>`     | `mail` no, `mailtest` auto | Copy in the sender's mail file: `yes`, `no` or `auto`      |
| `-mailnab <file>`       | `names.nsf`             | The Domino Directory to search                                |
| `-mailsize <KB>`        | `1-20`                  | Mail body size in KB: a number or a range                     |
| `-mailtext <style>`     | `lorem`                 | Mail body text: `lorem` or `funny`                            |
| `-mailattach <n>`       | `0-2`                   | Attachments per mail (0-20): a number or a range              |
| `-mailattachsize <KB>`  | `10-200`                | Size of one attachment in KB: a number or a range             |
| `-mailattachtype <t>`   | `binary`                | Attachment content: `binary` (random bytes) or `text`         |
| `-nowait`               | off                     | Do not wait for a free account: end when none is left         |
| `-poll <seconds>`       | `2`                     | Polling interval; also the time between two job steps         |

### Two ways to write an option

Every option can be written in the classic form or in the URL style, and the two can be mixed:

```
load domlem -coordinator http://coordinator:8788 -switch -mailsize 20-50
load domlem coordinator=http://coordinator:8788&switch&mailsize=20-50
```

The URL style has no blanks (`name=value&name&name=value`, a bare name is a flag), so the same string can be used on the
command line and as the parameters of a job (below). A value with a blank, `&` or `=` inside is written with `%20`, `%26`
and `%3D`; other characters are taken as they are. In a Unix shell put a URL style argument in single quotes, because of
the `&`. A value like a long filter formula is easier in the classic form: `-mailfilter 'Form="Person" & ...'`.

### Parameters of a job

The settings of a job are the command line defaults, and the parameters of the `run` command change them for that job
only. The next `run` starts from the defaults again. The parameters are the `params` field of the command, in the URL
style; send them with `--data-urlencode`:

```bash
curl -X POST -d 'target=all&command=run&job=mailtest' --data-urlencode 'params=mailsize=20-50&mailattach=0-5' $H/command
curl -X POST -d 'target=all&command=run&job=agent:mailread' --data-urlencode 'params=agenttimeout=60' $H/command
```

A job may only change **what** it does, never **where** it works or **whom** it reaches. Accepted: `agenttimeout`,
`mailrandom`, `mailsize`, `mailtext`, `mailattach`, `mailattachsize`, `mailattachtype`, `mailsubject`, `mailsentcopy` and `closesession`.
The database (`-db`), the agent (`-agent`; `agent:<name>` picks another agent of `-db`), the recipients (`-mailto`), the
people filter (`-mailfilter`, `-mailnab`), the identity, the server, the coordinator and the certifier stay what the
command line says, so a command over the network cannot point a lemming at other databases or other people. A refused
parameter makes the job fail with the reason (the lemming stays and waits).

Option names are not case sensitive. Numbers must be plain decimal numbers in range, otherwise the task refuses to
start: `-poll` 1-3600, `-agenttimeout` 0-86400, `-mailsize` 0-10240 KB, `-mailattach` 0-20, `-mailattachsize` 0-102400 KB,
`-mailrandom` 0-50. A range is `min-max`, for example `10-500`.

### Random recipients

`-mailrandom N` (or a range `min-max`) adds random people to every mail, picked from the person documents of `-mailnab`.
Which people:

- **Without `-mailfilter`: the test pool of the lemming's own account.** The short name of the account without its
  trailing digits is the pool prefix (`load000003` gives `load`), and only person documents whose short name is that
  prefix followed by digits are picked: `Form = "Person" & @Matches(ShortName; "load{0-9}+{0-9}")`. So a load test never
  mails a real user, and nothing has to be configured. A short name without such a prefix (no trailing digits, or other
  characters than letters, digits, `.`, `-`, `_`) makes the job fail: then give `-mailfilter`.
- **With `-mailfilter`:** your selection formula, for example `Form="Person" & @Contains(ShortName; "test")`. Make sure
  it selects test users only.

The matching full names are read once, with the first mail (up to 20000), and every mail picks from that list. They are
added to the `-mailto` recipients. The filter runs with the identity of the lemming: it must be able to read the person
documents.

## How it works

```
  nshtestherd (coordinator)             domlem process (one per client)
  -------------------------             -------------------------------
  account pool, commands      <-----    HerdClient (shared core: HTTP, register, poll, commands)
  /status, /metrics                       |
                                          +-- DomlemHooks: log, wait (AddInIdleDelay), identity, job table
                                               |
                                               +-- -switch: userreg.cpp -> vault ID -> switch user
                                               +-- DominoLoad: owns the handles, one Op...() per test
                                                    +-- OpDbOpen
                                                    +-- OpRunAgent  -> AgentRunner
                                                    +-- OpSendMail  -> MailClient -> TextGenerator
```

- The coordinator decides everything. `run <job>` starts a job, `pause` stops the steps for the given time, `stop` ends
  the process (it reports `done`), a server `quit` ends it too. The worker contract (registration with a request key,
  polling, acknowledgements, job results, `done` and `error` reporting, lost allocations) is in the shared core, the
  same as for the generic `nshtestherd --runner`.
- One step per poll while the worker is `running`. A step that fails ends the job, not the worker: the job is reported as
  `failed` with the reason, the handles are closed, and the worker is `idle` again. An unknown job name or invalid job
  parameters fail the same way. `error` (which ends the task) is left for a worker that cannot work at all: the
  identity setup failed. The result of the last step is shown by the coordinator: `curl "$H/client?test_id=N"`.
- No database stays open between two operations, as a user closes a database when done with it. Every operation opens
  what it needs and releases it in the reverse order: notes before their database, the agent before its database, the
  database last, with its session when `-closesession` is on (the default). So every step connects anew:

  | Job               | Once per job (read, closed again)                       | Every operation                                                          |
  | ----------------- | ------------------------------------------------------- | ------------------------------------------------------------------------ |
  | `dbopen`          | -                                                       | open `-dbopen`, read the access level, close                             |
  | `agent`           | logical name of `-db` (open, name, close)               | open `-db`, open the agent, run it, close the agent, close `-db`         |
  | `mail`, `mailtest`| recipient list from the directory; sender's mail file (lookup) | sent copy: open the mail file, write the note, close note and file; then the same with `mail.box` |

  The directory is read and closed before a job opens the databases it works in. All operations run with the identity
  the process has: with `-switch` after the switch.

| File                             | Purpose                                                                          |
| -------------------------------- | -------------------------------------------------------------------------------- |
| `domlem.cpp`                     | The task: options, Notes hooks (identity, job, log, wait), `AddInMain`           |
| `dominoload.cpp`, `dominoload.h` | `DominoLoad`: owns the handles, one method per test; add new tests here          |
| `agentrun.cpp`, `agentrun.h`     | `AgentRunner`: opens an agent once, runs it with a run context per run           |
| `mailclient.cpp`, `mailclient.h` | `MailClient`: crafts a full mail note, writes it into `mail.box`                 |
| `textgen.cpp`, `textgen.h`       | `TextGenerator`: body text, random attachment data                               |
| `userreg.cpp`, `userreg.h`       | `RegEnsureUser`: looks a user up, registers it with mail file when missing       |
| `lib.cpp`, `lib.h`               | Shared helpers: strings, number and range parsing, directory and document search |
| `../src/herdclient.*`            | The shared client core (+ `httpclient`, `wire`), compiled into `herd_*.o`        |

## Settings from the server: token, certifier password, defaults

Secrets are never options (they would show up in the process list and in `show tasks`):

| Name            | What                                                                              |
| --------------- | --------------------------------------------------------------------------------- |
| `NSHTEST_TOKEN` | The coordinator's token, the same variable as for the coordinator and the runner  |
| `DOMLEM_COORDINATOR` | Not a secret: the coordinator for every lemming of the server (`-coordinator` overrides it) |
| `DOMLEM_SWITCH` | Not a secret: `1` makes every lemming of the server work as its own user, like `-switch`; `0` forbids it, also when the coordinator's worker options ask for it; `switch=0` on the command line turns it off for one lemming |
| `DOMLEM_TOKEN`  | Optional: a token for the lemmings only; wins over `NSHTEST_TOKEN` when both are set |
| `DOMLEM_CERTPW` | Password of the certifier ID file (`-certid`)                                     |

domlem reads them first from the environment of the server process (set them before the server starts, for example in
the container's environment), else from `notes.ini` (`set config NSHTEST_TOKEN=...`). The environment is the better
place: in `notes.ini` the value is plain text for everybody who can read the file or run `show config`.

Without a token on the coordinator, no token is needed. With one, a lemming without it is refused at registration and
ends (the log says "registration refused: missing or wrong token").

## What domlem writes where

| Where                                  | What                                                    | When                          |
| -------------------------------------- | ------------------------------------------------------- | ----------------------------- |
| Domino Directory (`names.nsf`)         | The person document of a test user (`REGNewPerson`)     | `-switch`, user missing       |
| ID vault, mail directory               | The new user's ID and mail file (`REGNewPerson`)        | `-switch`, user missing       |
| `mail.box` of the server               | The messages of the mail jobs                           | `mail`, `mailtest`            |
| The sender's own mail file             | The sent copies                                         | `mailtest`, `-mailsentcopy`   |
| `-db` (a test database, never a system database) | Whatever the agent does                       | `agent`, `agent:<name>`       |
| Notes data directory                   | `domlem_*` temporary files (ID, attachment data)        | `-switch`, attachments        |

Everything else only reads: the lookups of users and mail files, the people search for random recipients (`-mailnab`),
and the `dbopen` job (open, access level, close). The registration of a test user is the **only** write into the
directory; a new write path there (for example test groups) will be an explicit, documented option.

## Files and security

- **Passwords:** account passwords come from the coordinator and are only used to download and open the ID. They are
  never logged, and the buffers are cleared. The certifier password comes from `DOMLEM_CERTPW`, never from an option.
- **Files:** domlem only writes into the Notes data directory, and every name starts with `domlem_`. In `-switch` mode a
  private ID file `domlem_<test_id>_<pid>.id` (removed at the end), and for attachments temporary files
  `domlem_att_<pid>_<mail>_<n>.tmp` (removed right after they are attached). A crash can leave such files behind: every
  lemming removes them when it starts, but only those whose process `<pid>` no longer exists, so the files of the
  lemmings that are running stay (Linux). The test id and the short name are checked before they become part of a file
  name.
- **Mail:** a load test cannot reach real users by accident: random recipients come from the account's test pool unless
  `-mailfilter` says otherwise, and the default recipient of the `mail` job is the sender.

## Limits

- A mail counts as *submitted* once it is in the `mail.box`. Whether and when the router delivered it is not measured.
- `stop`, `quit` and a server shutdown are noticed between two operations. A running agent (bounded by `-agenttimeout`),
  a database open or a registration is not interrupted. Calls to the coordinator time out after 3 seconds.
- A `run` command can change what a job contains (sizes, counts, subject, ...), not where it works or whom it mails:
  database, agent list, recipients and people filter come from the command line of the process (see "Parameters of a job").
- A user registered just now may need a moment until its ID is in the vault: the download is retried for about one
  minute (only for a new user), then the worker ends in `error`. An existing user is taken as it is; the log warns when
  it has no mail file in the directory (its mail jobs then cannot deliver or keep sent copies).
- Not implemented yet: reading mail, a separate builder role (provision users without working as them), `CopyTo` and
  `BlindCopyTo` options, delivery measurement.
- The attachment hotspot records have not been compared with a message of a real Notes client yet.

## Troubleshooting

| What you see                                 | Likely cause                                                  |
| -------------------------------------------- | ------------------------------------------------------------- |
| Task ends at once, usage in the log          | An option is invalid or out of range; the log names it        |
| Job `failed`: "Cannot open database"         | `-dbopen` (dbopen) or `-db` (agent) is missing, or the identity has no access |
| Job `failed`: "does not run agents of the system database" | `-db` is a system database: use a test database |
| Job `failed`: "Agent [...] not found"        | Wrong agent name, or the agent is not in `-db`                |
| Mail submitted, nothing arrives (no -switch) | The recipient is the server itself: use `-mailto` or -switch  |
| `-switch`: "Cannot find or register"         | No Domino CA for the server's organization: give `-ca`, or `-certid` + `DOMLEM_CERTPW` |
| Worker `error`: "Cannot download the ID"     | No ID in the vault (also not after the retries for a new user), wrong password, or the policy does not put IDs in the vault (`-policy`) |
| "Cannot derive the test user filter"         | The account's short name has no prefix + digits: `-mailfilter`|
| "No person matches the filter"               | The filter matches nobody, or the person documents are hidden |
| "No mail file for [...]" (`-mailsentcopy yes`) | The sender has no mail file: use `-switch`, or `auto`/`no`  |
| Worker ends with "lost"                      | The coordinator was restarted: stop the workers first         |

## Build

Needs the Notes C API (`LOTUS` and `Notes_ExecDirectory` set, as for your other add-ins) and g++ with C++17:

```bash
cd domlem
make              # compiles the client core sources of ../src (herd_*.o) and domlem, with the Notes toolchain
make install      # copies domlem to the Domino program directory
```

The sources of the shared client core (`../src/httpclient.cpp`, `wire.cpp`, `herdclient.cpp`) are compiled by domlem's
own makefile into `herd_*.o`, with the Notes toolchain. The objects of the repository's main build are not used: they
come from another compiler or glibc and do not link in the Notes build environment.
