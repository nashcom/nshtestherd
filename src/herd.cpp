// herd.cpp - in-memory coordinator state

#include "herd.h"

namespace
{

const char *STATE_NAMES[STATE_COUNT] = { "registered", "idle", "running", "paused", "stopping", "done", "error" };

const char *JOB_RESULT_NAMES[JOB_RESULT_COUNT] = { "", "ok", "failed", "stopped" };

Result Fail(int code, const char *error, const std::string &message)
{
    Result r;
    r.code    = code;
    r.error   = error;
    r.message = message;
    return r;
}

} // namespace

const char *StateName(ClientState state)
{
    return STATE_NAMES[(int)state];
}

bool IsTerminal(ClientState state)
{
    return (ClientState::Done == state) || (ClientState::Error == state);
}

bool ParseWorkerState(const std::string &name, ClientState &state)
{
    for (int i = 1; i < STATE_COUNT; i++)
    {
        if (name == STATE_NAMES[i])
        {
            state = (ClientState)i;
            return true;
        }
    }

    return false;
}

const char *CommandName(CommandKind kind)
{
    switch (kind)
    {
        case CommandKind::Run: return "run";
        case CommandKind::Pause: return "pause";
        case CommandKind::Stop: return "stop";
        default: return "idle";
    }
}

bool ParseCommandName(const std::string &name, CommandKind &kind)
{
    for (CommandKind k : { CommandKind::Idle, CommandKind::Run, CommandKind::Pause, CommandKind::Stop })
    {
        if (name == CommandName(k))
        {
            kind = k;
            return true;
        }
    }

    return false;
}

const char *JobResultName(JobResult result)
{
    return JOB_RESULT_NAMES[(int)result];
}

// A worker reports only real results: "ok", "failed" or "stopped"
bool ParseJobResult(const std::string &name, JobResult &result)
{
    for (int i = 1; i < JOB_RESULT_COUNT; i++)
    {
        if (name == JOB_RESULT_NAMES[i])
        {
            result = (JobResult)i;
            return true;
        }
    }

    return false;
}

Herd::Herd() : started_(std::chrono::steady_clock::now())
{
}

ClientView Herd::ViewOf(int testId, const Client &client) const
{
    ClientView v;
    v.testId       = testId;
    v.user         = pool_[client.userIndex];
    v.instruction  = client.instruction;
    v.state        = client.state;
    v.ackCommandId = client.ackCommandId;
    v.message      = client.message;
    v.lastJobId      = client.lastJobId;
    v.lastJob        = client.lastJob;
    v.lastJobResult  = client.lastJobResult;
    v.lastJobMessage = client.lastJobMessage;
    v.ageSeconds   = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - client.lastContact).count();
    return v;
}

Result Herd::LoadCsv(const std::string &text, size_t &userCount)
{
    std::vector<UserRecord> users;
    std::string             err;

    if (!ParseUsersCsv(text, users, err))
    {
        std::lock_guard<std::mutex> lock(mutex_);
        csvLoadFailures_++;
        return Fail(400, "invalid_csv", err);
    }

    return LoadUsers(std::move(users), userCount);
}

Result Herd::LoadUsers(std::vector<UserRecord> &&users, size_t &userCount)
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (!clients_.empty())
    {
        csvLoadFailures_++;
        return Fail(409, "load_not_allowed", "clients have already registered; restart the service to load a new account pool");
    }

    pool_      = std::move(users);
    nextFree_  = 0;
    userCount  = pool_.size();
    csvLoads_++;
    return Result();
}

Result Herd::Register(const std::string &requestKey, bool &isNew, ClientView &out)
{
    std::lock_guard<std::mutex> lock(mutex_);

    isNew = false;

    if (!requestKey.empty())
    {
        auto it = byKey_.find(requestKey);

        if (it != byKey_.end())
        {
            Client &c     = clients_[it->second];
            c.lastContact = std::chrono::steady_clock::now();
            out           = ViewOf(it->second, c);
            return Result();
        }
    }

    if (pool_.empty())
    {
        allocationFailures_++;
        return Fail(409, "no_accounts_loaded", "no account pool has been loaded");
    }

    if (nextFree_ >= pool_.size())
    {
        allocationFailures_++;
        return Fail(409, "pool_exhausted", "all accounts are allocated");
    }

    int    testId = nextId_++;
    Client c;
    c.userIndex   = nextFree_++;
    c.requestKey  = requestKey;
    c.lastContact = std::chrono::steady_clock::now();
    pool_[c.userIndex].allocated = true;

    if (!requestKey.empty())
        byKey_[requestKey] = testId;

    Client &stored = clients_[testId] = c;
    registrations_++;
    isNew = true;
    out   = ViewOf(testId, stored);
    return Result();
}

void Herd::SetWorkerOptions(const std::string &options)
{
    std::lock_guard<std::mutex> lock(mutex_);
    workerOptions_ = options;
}

std::string Herd::WorkerOptions()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return workerOptions_;
}

Result Herd::ReportStatus(const StatusReport &report, ClientView &out)
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = clients_.find(report.testId);

    if (it == clients_.end())
        return Fail(404, "unknown_test_id", "no such test_id");

    Client &c = it->second;

    if (report.hasAck && report.ackCommandId > c.instruction.commandId)
        return Fail(400, "invalid_ack_command_id", "ack_command_id is newer than the issued command_id");

    if (report.hasLastJob && report.lastJobId > c.instruction.commandId)
        return Fail(400, "invalid_last_job_id", "last_job_id is newer than the issued command_id");

    c.lastContact = std::chrono::steady_clock::now();
    statusReports_++;

    // A finished worker stays finished; late reports only refresh contact time.
    if (!IsTerminal(c.state))
    {
        c.state = report.state;

        if (report.hasAck && report.ackCommandId > c.ackCommandId)
            c.ackCommandId = report.ackCommandId;

        if (report.hasMessage)
            c.message = report.message;

        // The worker repeats its last result with every report: only a newer job counts (once)
        if (report.hasLastJob && report.lastJobId > c.lastJobId)
        {
            c.lastJobId      = report.lastJobId;
            c.lastJob        = report.lastJob;
            c.lastJobResult  = report.lastJobResult;
            c.lastJobMessage = report.hasMessage ? report.message : std::string();
            jobsEnded_[(int)report.lastJobResult]++;
        }
    }

    out = ViewOf(it->first, c);
    return Result();
}

Result Herd::SetCommand(bool all, int testId, const Instruction &command, size_t &updated, long long &commandId)
{
    std::lock_guard<std::mutex> lock(mutex_);

    updated   = 0;
    commandId = 0;

    auto apply = [&](Client &c)
    {
        c.instruction.kind         = command.kind;
        c.instruction.job          = (CommandKind::Run == command.kind) ? command.job : std::string();
        c.instruction.params       = (CommandKind::Run == command.kind) ? command.params : std::string();
        c.instruction.pauseSeconds = (CommandKind::Pause == command.kind) ? command.pauseSeconds : 0;
        c.instruction.commandId++;
        commandUpdates_++;
        updated++;
        commandId = c.instruction.commandId;
    };

    if (all)
    {
        for (auto &entry : clients_)
        {
            if (!IsTerminal(entry.second.state))
                apply(entry.second);
        }

        commandId = 0;
        return Result();
    }

    auto it = clients_.find(testId);

    if (it == clients_.end())
        return Fail(404, "unknown_test_id", "no such test_id");

    if (IsTerminal(it->second.state))
        return Fail(409, "client_finished", "client has finished and cannot be restarted");

    apply(it->second);
    return Result();
}

Result Herd::GetClient(int testId, ClientView &out)
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = clients_.find(testId);

    if (it == clients_.end())
        return Fail(404, "unknown_test_id", "no such test_id");

    out = ViewOf(it->first, it->second);
    return Result();
}

Summary Herd::GetSummary()
{
    Summary s;

    std::lock_guard<std::mutex> lock(mutex_);

    s.usersTotal        = pool_.size();
    s.usersAllocated    = nextFree_;
    s.clientsTotal      = clients_.size();
    s.registrations     = registrations_;
    s.allocationFailures = allocationFailures_;
    s.statusReports     = statusReports_;
    s.commandUpdates    = commandUpdates_;
    s.csvLoads          = csvLoads_;
    s.csvLoadFailures   = csvLoadFailures_;

    for (int i = 0; i < JOB_RESULT_COUNT; i++)
        s.jobsEnded[i] = jobsEnded_[i];

    s.uptimeSeconds     = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started_).count();

    for (const auto &entry : clients_)
        s.byState[(int)entry.second.state]++;

    return s;
}
