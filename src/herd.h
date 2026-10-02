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

struct Instruction
{
    CommandKind kind         = CommandKind::Idle;
    std::string job;
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
        std::chrono::steady_clock::time_point lastContact;
    };

    ClientView ViewOf(int testId, const Client &client) const;

    std::mutex                            mutex_;
    std::vector<UserRecord>               pool_;
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
    std::chrono::steady_clock::time_point started_;
};
