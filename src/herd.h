// herd.h - in-memory coordinator state: account pool, clients, commands, counters
//
// One mutex guards everything. No method touches a socket; callers get copies.

#pragma once

#include "csv.h"

#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <vector>

enum class ClientState
{
    Registered = 0,
    Idle,
    Running,
    Paused,
    Stopping,
    Done,
    Error
};

const int STATE_COUNT = 7;

const char *StateName(ClientState state);
bool        IsTerminal(ClientState state);

// Workers may report every state except "registered" (the initial state).
bool ParseWorkerState(const std::string &name, ClientState &state);

enum class CommandKind
{
    Idle,
    Run,
    Pause,
    Stop
};

const char *CommandName(CommandKind kind);
bool        ParseCommandName(const std::string &name, CommandKind &kind);

// How a job (one applied "run" command) ended
enum class JobResult
{
    None = 0,   // no job has ended yet
    Ok,         // finished by itself
    Failed,     // could not start, or failed
    Stopped     // ended by a newer command or by the worker shutting down
};

const int JOB_RESULT_COUNT = 4;

const char *JobResultName(JobResult result);
bool        ParseJobResult(const std::string &name, JobResult &result);

struct Instruction
{
    CommandKind kind         = CommandKind::Idle;
    std::string job;
    std::string params;           // run only: job parameters, "name=value&name" (empty: none)
    long long   pauseSeconds = 0;
    long long   commandId    = 0;
};

struct ClientView
{
    int         testId = 0;
    UserRecord  user;
    Instruction instruction;
    ClientState state        = ClientState::Registered;
    long long   ackCommandId = 0;
    long long   ageSeconds   = 0; // since last contact
    std::string message;

    // The last job that ended (lastJobId 0: none yet)
    long long   lastJobId     = 0;
    std::string lastJob;
    JobResult   lastJobResult = JobResult::None;
    std::string lastJobMessage;
};

struct Result
{
    int         code = 200;
    std::string error;
    std::string message;

    bool Ok() const { return code < 300; }
};

struct StatusReport
{
    int         testId = 0;
    ClientState state  = ClientState::Idle;
    bool        hasAck = false;
    long long   ackCommandId = 0;
    bool        hasMessage = false;
    std::string message;

    // The last job that ended, sent again with every report: counted once, when lastJobId is new
    bool        hasLastJob    = false;
    long long   lastJobId     = 0;
    std::string lastJob;
    JobResult   lastJobResult = JobResult::None;
};

struct Summary
{
    size_t             usersTotal     = 0;
    size_t             usersAllocated = 0;
    size_t             clientsTotal   = 0;
    size_t             byState[STATE_COUNT] = { 0 };
    unsigned long long registrations     = 0;
    unsigned long long allocationFailures = 0;
    unsigned long long statusReports     = 0;
    unsigned long long commandUpdates    = 0;
    unsigned long long csvLoads          = 0;
    unsigned long long csvLoadFailures   = 0;
    unsigned long long jobsEnded[JOB_RESULT_COUNT] = { 0 }; // by JobResult (None stays 0)
    long long          uptimeSeconds     = 0;
};

class Herd
{
public:
    Herd();

    // Replaces the pool (409 once any client has registered). LoadCsv parses and
    // validates completely before swapping; generated pools use LoadUsers directly.
    Result LoadUsers(std::vector<UserRecord> &&users, size_t &userCount);
    Result LoadCsv(const std::string &text, size_t &userCount);

    // Non-empty requestKey makes the call idempotent. isNew tells 201 from 200.
    Result Register(const std::string &requestKey, bool &isNew, ClientView &out);

    // Worker options (--worker-options): one opaque string for every worker, returned with each registration. The
    // coordinator does not interpret it; a worker takes what it knows (domlem: "switch") and ignores the rest.
    void        SetWorkerOptions(const std::string &options);
    std::string WorkerOptions();

    // Updates reported state/ack/contact time; returns the current instruction.
    Result ReportStatus(const StatusReport &report, ClientView &out);

    // Sets an instruction for one client (all=false) or for every client that
    // exists now and is not finished (all=true). Each affected client gets a
    // new command_id. commandId is only meaningful for a single target.
    Result SetCommand(bool all, int testId, const Instruction &command, size_t &updated, long long &commandId);

    Result GetClient(int testId, ClientView &out);

    Summary GetSummary();

private:
    struct Client
    {
        size_t      userIndex = 0;
        std::string requestKey;
        Instruction instruction;
        ClientState state        = ClientState::Registered;
        long long   ackCommandId = 0;
        std::string message;
        long long   lastJobId     = 0;
        std::string lastJob;
        JobResult   lastJobResult = JobResult::None;
        std::string lastJobMessage;
        std::chrono::steady_clock::time_point lastContact;
    };

    ClientView ViewOf(int testId, const Client &client) const;

    std::mutex                            mutex_;
    std::vector<UserRecord>               pool_;
    std::string                           workerOptions_;
    size_t                                nextFree_ = 0;
    std::map<int, Client>                 clients_;
    std::map<std::string, int>            byKey_;
    int                                   nextId_ = 1;
    unsigned long long                    registrations_      = 0;
    unsigned long long                    allocationFailures_ = 0;
    unsigned long long                    statusReports_      = 0;
    unsigned long long                    commandUpdates_     = 0;
    unsigned long long                    csvLoads_           = 0;
    unsigned long long                    csvLoadFailures_    = 0;
    unsigned long long                    jobsEnded_[JOB_RESULT_COUNT] = { 0 };
    std::chrono::steady_clock::time_point started_;
};
