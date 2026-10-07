# domlem - the Domino Performance Test Lemming

**domlem** is a Domino add-in task that generates real load on a Domino server. It works as an
[nshtestherd](../README.md) worker: it registers with the nshtestherd coordinator, receives a test account, and then only
follows the commands the coordinator sends (`run <job>`, `pause`, `idle`, `stop`). Like a lemming it does what it is
told, and you start many of them.

The work itself is real Notes API work, done in the task's own process:

| Job      | What one step does                                                                                  |
| -------- | --------------------------------------------------------------------------------------------------- |
| `dbopen` | Opens a database as the current user, reads the access level, closes it again                       |
| `agent`  | Runs a Domino agent of a database                                                                   |
| `mail`   | Crafts a complete mail (text, attachments with icons) and writes it into the mail.box of the server |

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

`show tasks` shows `domlem` with the status line `Registered, identity: ...`. The coordinator shows the new client:

```bash
H=http://127.0.0.1:8788
curl $H/status
```

**4. Tell it what to do.** Every command goes to the coordinator, not to the lemming:

```bash
curl -X POST -d 'target=all&command=run&job=dbopen' $H/command     # start the job
curl "$H/client?test_id=1"                                         # what it did: "dbopen #12 ... access level 6"
curl -X POST -d 'target=all&command=pause&pause_seconds=30' $H/command
curl -X POST -d 'target=all&command=stop' $H/command              # the lemming reports done and ends
```

That is all for a first look. The `dbopen` job opens `nshtestherd.nsf`: create an empty database of that name on the
server first, or use `-db names.nsf` (it always exists).

## Recipes

Each recipe is one command line for the lemming (`load domlem ...`) and one for the coordinator.

**Smoke test of the connection** - nothing to prepare except a database:

```
load domlem -db names.nsf
curl -X POST -d 'target=all&command=run&job=dbopen' $H/command
```

**Mail load to a real test user** - every mail goes to that user (the server identity sends it):

```
load domlem -mailto "CN=Test User/O=Org"
curl -X POST -d 'target=all&command=run&job=mail' $H/command
```

**Mail load where every lemming is its own user** - mail goes to the lemming's own mail file. The users and their mail
files are created when missing; this needs a certifier (`-ca` for a Domino CA, or `-certid` for an ID file):

```
load domlem -switch -ca "/O=Org"
curl -X POST -d 'target=all&command=run&job=mail' $H/command
```

**Mail load between the test users** - every mail goes to 2 random test users. The filter is required: it is what keeps
the load away from real people.

```
load domlem -switch -ca "/O=Org" -mailrandom 2 -mailfilter 'Form="Person" & @Contains(ShortName; "test")'
```

**Bigger mails** - bodies of 20-50 KB and 0-5 attachments of 100-1000 KB:

```
load domlem -mailto "CN=Test User/O=Org" -mailsize 20-50 -mailattach 0-5 -mailattachsize 100-1000
```

**Agent load** - runs the agent `TestAgent` of `nshtestherd.nsf` (create the agent in Designer, see
[The agent job](#the-agent-job)):

```
load domlem
curl -X POST -d 'target=all&command=run&job=agent' $H/command
```

**Many lemmings** - one process is one client with one identity, so start the task many times. A shell loop on the server
is the safe way (it does not depend on the console accepting the same task name repeatedly):

```bash
for i in $(seq 1 20)
do
  /opt/hcl/domino/notes/latest/linux/domlem -switch -ca "/O=Org" &
done
```

Every process registers itself with a unique request key and receives its own account and `test_id`. Running the binary
directly needs the Domino environment of the server user, as for any add-in.

## What a default mail looks like

You do not have to set anything to get realistic mail. The defaults are:

| Property            | Default                             | Option                                              |
| ------------------- | ----------------------------------- | --------------------------------------------------- |
| Recipient           | The sender itself                   | `-mailto`, `-mailrandom`                            |
| Subject             | `domlem load test #<n>`             | (fixed)                                             |
| Body text           | 1-20 KB of lorem ipsum              | `-mailsize`, `-mailtext` (lorem or funny)           |
| Attachments         | 0-2 per mail, 10-200 KB             | `-mailattach`, `-mailattachsize`, `-mailattachtype` |
| Attachment content  | Random bytes (they do not compress) | `-mailattachtype text` for compressible text        |
| Marked as generated | Yes (`Auto-submitted`)              | (fixed)                                             |

Every mail takes its own size, attachment count and attachment sizes out of the ranges, evenly distributed. The values are
made from the mail's number, so the same message number has the same content on every run. The job message shows what
the mail contained, for example `mail #17 submitted, 8200 bytes text, 2 attachments with 410000 bytes`.

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

In `-switch` mode, once per lemming, right after the coordinator has allocated an account:

1. **Lookup:** the user is searched in the Domino Directory by short name, internet address and full name.
2. **Registration:** only if the user does not exist, it is registered with the certifier (`-ca` or `-certid`): ID,
   directory entry, internet password and the mail file (`mail/<shortname>.nsf` on the server) in one step.
3. **ID:** the ID is downloaded from the ID vault into a private file in the data directory (`SECidfGet`).
4. **Switch:** the process switches to that ID (`SECKFMSwitchToIDFile`, the process-wide identity). The ID file is
   removed when the process ends.

Existing users are reused as they are: nothing is registered and no ID or password is changed. Registering needs a
certifier:

| Option            | Default        | Meaning                                                                        |
| ----------------- | -------------- | ------------------------------------------------------------------------------ |
| `-ca <name>`      | none           | Domino CA that certifies the new users (used without certifier ID + password)  |
| `-certid <file>`  | none           | Certifier ID file, used directly when `DOMLEM_CERTPW` holds its password       |
| `-template <ntf>` | server default | Mail template for the new mail files, for example `mail14.ntf`                 |
| `-maildir <dir>`  | `mail`         | Directory of the new mail files (`<dir>/<shortname>.nsf`)                      |
| `-policy <name>`  | none           | Explicit policy for new users, for example the one that puts the ID in a vault |

The certifier password is deliberately not an option: it would show up in the process list and in `show tasks`. Set the
environment variable `DOMLEM_CERTPW` instead.

## The agent job

`run job=agent` runs one agent of `-db` (default `nshtestherd.nsf`) per step, as the current identity. The agent is opened
once per job; every run has its own run context with the time limit.

- Create the agent in Designer in `nshtestherd.nsf`: name `TestAgent` (or give another name with `-agent`), any trigger
  that allows to run it manually or in the background, no user interface.
- Sign it with an ID that may run agents on the server.
- The agent runs unrestricted: the signer's agent privileges are not checked (no `AGENT_SECURITY_ON`), only the access
  of the current identity to the database counts.
- `-agenttimeout` (default 600 seconds, 0: none) limits one run.

## Options

| Option                  | Default                 | Meaning                                                       |
| ----------------------- | ----------------------- | ------------------------------------------------------------- |
| `-coordinator <url>`    | `http://127.0.0.1:8788` | nshtestherd coordinator                                       |
| `-server <name>`        | this server             | Domino server to work against (databases, ID vault, mail.box) |
| `-switch`               | off                     | Work as the account's own user (see the two identity modes)   |
| `-db <path>`            | `nshtestherd.nsf`       | Database used by the `dbopen` and `agent` jobs                |
| `-agent <name>`         | `TestAgent`             | Agent run by the `agent` job (in the `-db` database)          |
| `-agenttimeout <sec>`   | `600`                   | Execution limit of one agent run in seconds, 0: none          |
| `-mailto <names>`       | the sender              | Recipients of the `mail` job, comma separated                 |
| `-mailrandom <count>`   | `0`                     | Random recipients per mail (needs `-mailfilter`)              |
| `-mailfilter <formula>` | none                    | Selection formula of the people to pick from                  |
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
only. The next `run` starts from the defaults again. Only the job level options are accepted there: `db`, `agent`,
`agenttimeout`, `mailto`, `mailrandom`, `mailfilter`, `mailnab`, `mailsize`, `mailtext`, `mailattach`, `mailattachsize`
and `mailattachtype`. A command can never change the identity, the server, the coordinator or the certifier. A refused
parameter makes the job fail with the reason.

domlem is ready for this, but the coordinator does not carry the parameters yet. Planned: `run` gets the parameters as
extra fields, `curl -X POST -d 'target=all&command=run&job=mail&mailsize=20-50' $H/command`, and later the lemmings can
ask nshtestherd for their commands.

Option names are not case sensitive. Numbers must be plain decimal numbers in range, otherwise the task refuses to
start: `-poll` 1-3600, `-agenttimeout` 0-86400, `-mailsize` 0-10240 KB, `-mailattach` 0-20, `-mailattachsize` 0-102400 KB,
`-mailrandom` 0-50. A range is `min-max`, for example `10-500`.

**Random recipients:** `-mailrandom N` adds N random people to every mail, picked from the person documents of
`-mailnab` that match `-mailfilter`, a selection formula such as `Form="Person" & @Contains(ShortName; "test")`. The
filter is required, so that a load test never mails real users; without it the task refuses to start. The matching full
names are read once, with the first mail (up to 20000), and every mail picks from that list. They are added to the
`-mailto` recipients. The filter runs with the identity of the lemming: it must be able to read the person documents.

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
  polling, acknowledgements, `done` and `error` reporting, lost allocations) is in the shared core, the same as for the
  generic `nshtestherd --runner`.
- One step per poll while the worker is `running`. A step that fails ends the worker in `error`; an unknown job name too.
  The result of the last step is shown by the coordinator: `curl "$H/client?test_id=N"`.
- The Notes handles are opened when the job starts, with the identity the process has then: with `-switch` after the
  switch. `dbopen` opens a new connection in every step; the agent and the mail box stay open for the whole job.

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

## Files and security

- **Passwords:** account passwords come from the coordinator and are only used to download and open the ID. They are
  never logged, and the buffers are cleared. The certifier password comes from `DOMLEM_CERTPW`, never from an option.
- **Files:** domlem only writes into the Notes data directory, and every name starts with `domlem_`. In `-switch` mode a
  private ID file `domlem_<test_id>_<pid>.id` (removed at the end), and for attachments temporary files
  `domlem_att_<pid>_<mail>_<n>.tmp` (removed right after they are attached). A crash can leave such files behind; they
  are safe to delete. The test id and the short name are checked before they become part of a file name.
- **Mail:** a load test cannot reach real users by accident: random recipients need an explicit filter, and the default
  recipient is the sender.

## Limits

- A mail counts as *submitted* once it is in the `mail.box`. Whether and when the router delivered it is not measured.
- `stop`, `quit` and a server shutdown are noticed between two operations. A running agent (bounded by `-agenttimeout`),
  a database open or a registration is not interrupted. Calls to the coordinator time out after 3 seconds.
- The settings come from the command line of the process. The coordinator's `run` command only carries the job name until
  the parameters of a job are passed through (domlem already applies them, see "Parameters of a job").
- Subject and attachment names contain the mail's number only; they do not tell the workers apart.
- Registration does not wait for the vault: if the ID is not available right after the registration the worker ends in
  `error`. Existing users are not checked for a complete mail file.
- Not implemented yet: reading mail, a separate builder role (provision users without working as them), `CopyTo` and
  `BlindCopyTo` options, delivery measurement.
- The attachment hotspot records have not been compared with a message of a real Notes client yet.

## Troubleshooting

| What you see                                 | Likely cause                                                  |
| -------------------------------------------- | ------------------------------------------------------------- |
| Task ends at once, usage in the log          | An option is invalid or out of range; the log names it        |
| Worker `error`: "Cannot open database"       | `-db` is missing on the server, or the identity has no access |
| Worker `error`: "Agent [...] not found"      | Wrong agent name, or the agent is not in `-db`                |
| Mail submitted, nothing arrives (no -switch) | The recipient is the server itself: use `-mailto` or -switch  |
| `-switch`: "Cannot find or register"         | No certifier (`-ca`, or `-certid` + `DOMLEM_CERTPW`)          |
| Worker `error`: "Cannot download the ID"     | No ID in the vault for the user yet, or wrong password        |
| "-mailrandom needs -mailfilter"              | Random recipients are refused without a test user filter      |
| "No person matches the filter"               | The filter matches nobody, or the person documents are hidden |
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
