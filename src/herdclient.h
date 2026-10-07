// herdclient.h - the worker side of the nshtestherd protocol, without any job or platform specifics
//
// One HerdClient is one client of the coordinator: it registers (with a request key, so a retry returns the same
// account), polls /status, reports its state, applies each command_id exactly once (run / pause / stop / idle)
// and keeps desired command and reported state apart. What a "run" command actually DOES, how to wait and how to log
// is supplied by the program through HerdHooks:
//   - the generic runner starts child processes (or the built-in dummy job),
//   - domlem does short Notes operations in its own process.
//
// Pure C++17 on top of httpclient + wire: no Notes headers, no threads of its own. Link the objects
// (httpclient.o, wire.o, herdclient.o) into any program.

#pragma once

#include "httpclient.h"

#include <string>

struct HerdAccount
{
    std::string testId;          // decimal test_id assigned by the coordinator
    std::string firstName;
    std::string lastName;
    std::string password;
    std::string shortName;
    std::string internetAddress;
};

// What a job gets when a "run" command is applied
struct HerdJobContext
{
    HerdAccount account;
    std::string job;             // job name from the run command
    std::string params;          // parameters of the run command, "name=value&name" (empty if there are none)
    long long   commandId = 0;
    std::string serverUrl;       // coordinator URL
};

enum HerdJobResult
{
    HERD_JOB_RUNNING,            // keep going (the client stays "running")
    HERD_JOB_FINISHED,           // done, the client goes back to "idle"
    HERD_JOB_FAILED              // failed: the client reports "error" (final)
};

enum HerdRegistration
{
    HERD_REG_OK,
    HERD_REG_EXHAUSTED,          // no free account left
    HERD_REG_FAILED              // refused for another reason, or interrupted
};

enum HerdEnd
{
    HERD_END_CLEAN,              // stop command applied, or the program was interrupted
    HERD_END_POOL_EXHAUSTED,     // registration found every account booked
    HERD_END_FAILED              // error state, lost allocation, refused registration, identity setup failed
};

// Everything the program provides to a HerdClient
class HerdHooks
{
public:
    virtual ~HerdHooks() {}

    // ---- environment ----

    // True when the program wants to end (Ctrl+C, shutdown request, ...)
    virtual bool Stopping() = 0;

    // Waits about ms milliseconds; may return early when Stopping() becomes true
    virtual void Sleep(int ms) = 0;

    virtual void Log(const std::string &testId, const std::string &text) = 0;

    // ---- registration ----

    // Called when the outcome of the registration is known
    // The client is waiting to register: the coordinator is unreachable, has no pool yet, or (waitForAccount) has no
    // free account. Called on every attempt; the reason is a short text for a status line.
    virtual void Waiting(const std::string & /*reason*/) {}

    virtual void RegistrationDone(HerdRegistration /*outcome*/) {}

    // Called once after a successful registration, before the first command is followed. Prepare the identity here
    // (domlem: create the user, download the ID, switch to it). Return false with err to make the client report
    // "error" and end.
    virtual bool Registered(const HerdAccount & /*account*/, std::string & /*err*/) { return true; }

    // ---- job (a "run" command) ----

    // A run command was applied. message becomes the reported message. Return false with err when the job cannot
    // be started: the client then reports "error".
    virtual bool JobStart(const HerdJobContext &context, std::string &message, std::string &err) = 0;

    // True while the job must not be interrupted (for example a child process is running): newer commands wait
    // and are acknowledged once the job is over.
    virtual bool JobBusy() { return false; }

    // Called once per loop iteration while the client is "running": do one short step or check the job.
    // message is the current message; change it to report something else.
    virtual HerdJobResult JobStep(std::string &message) = 0;

    // Stop the job at once (the program is interrupted)
    virtual void JobAbort() {}
};

struct HerdClientConfig
{
    std::string serverUrl = "http://127.0.0.1:8788"; // coordinator, http://host[:port]
    std::string requestKey;                          // unique per client: a retry with the same key returns the same account
    std::string name      = "runner";                // names this program in the final message "<name> interrupted"
    int         pollMs       = 2000;                 // polling interval and registration retry interval
    int         startDelayMs = 0;                    // wait before the first registration (spreads many clients)
    bool        waitForAccount = true;               // no free account: keep asking (Waiting() tells why); false: end with the pool exhausted
    int         httpTimeoutSeconds = 10;             // connect and reply timeout of one call to the coordinator
};

class HerdClient
{
public:
    HerdClient(const HerdClientConfig &config, HerdHooks &hooks);

    // Registers, then follows the coordinator until a stop command or an interrupt. Blocks.
    HerdEnd Run();

    const HerdAccount &Account() const { return account_; }

private:
    struct Instruction
    {
        long long   id           = 0;
        std::string command      = "idle";
        std::string job;
        long long   pauseSeconds = 0;
    };

    HerdRegistration Register();
    bool             Report();
    bool             ReportFinal(const std::string &state, const std::string &message);
    bool             Apply();      // true when the client is finished (stop)
    void             Interrupted();
    void             Say(const std::string &text) { hooks_.Log(account_.testId, text); }

    HerdClientConfig config_;
    HerdHooks       &hooks_;
    HttpUrl          url_;

    HerdAccount account_;
    std::string state_   = "idle"; // observed state, as reported
    std::string message_;
    long long   applied_ = 0;      // last applied command_id, sent as ack_command_id
    Instruction latest_;           // last instruction received
    bool        lost_    = false;  // coordinator no longer knows this test_id

    std::string resumeState_ = "idle";
    long long   pauseEndMs_  = 0;  // steady clock, milliseconds
};
