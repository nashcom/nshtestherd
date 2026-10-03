// runner.cpp - optional runner: logical clients (one thread each) using the HTTP API

#include "runner.h"

#include "httpclient.h"
#include "process.h"
#include "wire.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <random>
#include <thread>

namespace
{

typedef std::chrono::steady_clock Clock;

std::mutex g_logMutex;

void Log(int index, const std::string &testId, const std::string &text)
{
    std::lock_guard<std::mutex> lock(g_logMutex);
    std::printf("[client %d%s%s] %s\n", index, testId.empty() ? "" : " id ", testId.c_str(), text.c_str());
    std::fflush(stdout);
}

struct Instr
{
    long long   id           = 0;
    std::string command      = "idle";
    std::string job;
    long long   pauseSeconds = 0;
};

// Outcome of a client's registration, published to the starter thread
enum RegState
{
    REG_PENDING = 0,
    REG_OK,
    REG_EXHAUSTED, // no free account left
    REG_FAILED     // refused for another reason, or interrupted
};

// Hard cap on logical clients (one thread each), also for fill mode
const int MAX_CLIENTS = 1000;

bool IsNumber(const std::string &s)
{
    return !s.empty() && s.size() <= 15 && std::string::npos == s.find_first_not_of("0123456789");
}

// A registration reply must carry a valid test_id, the whole account and the first instruction
bool CompleteRegistration(Form &f)
{
    if (!IsNumber(f["test_id"]) || "0" == f["test_id"] || f["shortname"].empty())
        return false;

    for (const char *key : { "firstname", "lastname", "password", "internetaddress", "command", "command_id" })
    {
        if (!f.count(key))
            return false;
    }

    return true;
}

class LogicalClient
{
public:
    // fill: started without --clients; the first client to find the pool booked ends cleanly
    // stop: the caller's interrupt flag; halt: set by the runner itself to shut every client down
    // (for example when a thread could not be started)
    LogicalClient(int index, const RunnerConfig &config, const HttpUrl &url, const std::string &runId, const std::atomic<bool> &stop, const std::atomic<bool> &halt, bool fill, std::atomic<int> &regState)
        : index_(index), config_(config), url_(url), runId_(runId), stop_(stop), halt_(halt), fill_(fill), regState_(regState)
    {
    }

    // Returns true when the client ended cleanly (stop command or runner interrupted).
    bool Run();

private:
    bool Register();
    bool Report();
    bool ReportFinal(const std::string &state, const std::string &message);
    bool Apply();             // true when the client is finished (stop)
    void CheckChild();
    void Interrupted();
    void Sleep(int ms);
    void Say(const std::string &text) { Log(index_, testId_, text); }
    bool Stopping() const { return stop_ || halt_; }

    int                        index_;
    const RunnerConfig        &config_;
    HttpUrl                    url_;
    std::string                runId_;
    const std::atomic<bool>   &stop_;
    const std::atomic<bool>   &halt_;
    bool                       fill_;
    std::atomic<int>          &regState_;

    std::string testId_;
    Form        account_;

    std::string state_   = "idle"; // observed state, as reported
    std::string message_;
    std::string job_;
    long long   applied_ = 0;      // last applied command_id, sent as ack_command_id
    Instr       latest_;           // last instruction received
    bool        lost_    = false;  // coordinator no longer knows this test_id

    std::string resumeState_ = "idle";
    Clock::time_point pauseUntil_;
    long long   dummyOps_ = 0;

    ChildProcess child_;
};

void LogicalClient::Sleep(int ms)
{
    // Sliced so a stop request is noticed quickly
    for (int waited = 0; waited < ms && !Stopping(); waited += 50)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

bool LogicalClient::Register()
{
    const std::string key = runId_ + "-" + std::to_string(index_);

    // Spread start-up so many clients do not hit the coordinator in one burst
    // (fill mode registers one client at a time already)
    if (!fill_)
        Sleep((index_ % 200) * 5);

    while (!Stopping())
    {
        HttpReply   reply;
        std::string err;

        if (HttpCall(url_, "POST", "/register", "request_key=" + UrlEncode(key), reply, err))
        {
            Form f;
            ParseTextFields(reply.body, f);

            if (200 == reply.status || 201 == reply.status)
            {
                // Never trust a reply that lacks the account: retrying with the same key
                // returns the same allocation, so an incomplete reply is safe to ask for again
                if (!CompleteRegistration(f))
                    Say("incomplete registration reply; retrying with the same request key");
                else
                {
                    account_ = f;
                    testId_  = f["test_id"];
                    Say("registered as " + f["shortname"]);
                    regState_ = REG_OK;
                    return true;
                }
            }
            // The pool may simply not be loaded yet; anything else is final
            else if (409 == reply.status && "no_accounts_loaded" == f["error"])
                Say("waiting: no account pool loaded yet");
            else if (409 == reply.status && "pool_exhausted" == f["error"])
            {
                Say("no free account left");
                regState_ = REG_EXHAUSTED;
                return false;
            }
            else if (503 != reply.status)
            {
                Say("registration refused: " + f["message"]);
                regState_ = REG_FAILED;
                return false;
            }
        }
        else
            Say("coordinator unreachable: " + err);

        Sleep(config_.pollMs);
    }

    regState_ = REG_FAILED;
    return false;
}

bool LogicalClient::Report()
{
    std::string body = "test_id=" + testId_ + "&state=" + state_ + "&ack_command_id=" + std::to_string(applied_) + "&message=" + UrlEncode(message_);

    HttpReply   reply;
    std::string err;

    if (!HttpCall(url_, "POST", "/status", body, reply, err))
    {
        Say("coordinator unreachable: " + err);
        return false;
    }

    Form f;
    ParseTextFields(reply.body, f);

    if (404 == reply.status)
    {
        lost_ = true;
        return false;
    }

    if (200 != reply.status)
    {
        Say("status report rejected: " + f["message"]);
        return false;
    }

    // A 200 without the instruction fields is a damaged reply: do not act on defaults
    if (!f.count("command_id") || !f.count("command") || !f.count("job") || !f.count("pause_seconds") || !IsNumber(f["command_id"]) || !IsNumber(f["pause_seconds"]))
    {
        Say("incomplete status reply; ignored");
        return false;
    }

    latest_.id           = std::atoll(f["command_id"].c_str());
    latest_.command      = f["command"];
    latest_.job          = f["job"];
    latest_.pauseSeconds = std::atoll(f["pause_seconds"].c_str());
    return true;
}

bool LogicalClient::ReportFinal(const std::string &state, const std::string &message)
{
    state_   = state;
    message_ = message;

    for (int attempt = 0; attempt < 3; attempt++)
    {
        if (Report())
            return true;

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    return false;
}

void LogicalClient::CheckChild()
{
    int code = 0;

    if (!child_.Finished(code))
        return;

    if (0 == code)
    {
        state_   = "idle";
        message_ = "job " + job_ + " finished (exit 0)";
    }
    else
    {
        state_   = "error";
        message_ = "job " + job_ + " failed (exit " + std::to_string(code) + ")";
    }

    Say(message_);
}

// Called only between child executions. Each command_id is applied exactly once.
bool LogicalClient::Apply()
{
    const Instr in = latest_;
    applied_       = in.id;

    if ("idle" == in.command)
    {
        state_   = "idle";
        job_.clear();
        message_ = "idle";
    }
    else if ("run" == in.command)
    {
        job_ = in.job;

        if (config_.program.empty())
        {
            state_   = "running";
            message_ = "dummy job " + job_;
        }
        else
        {
            EnvList env = {
                { "NSH_TEST_ID", testId_ },
                { "NSH_FIRSTNAME", account_["firstname"] },
                { "NSH_LASTNAME", account_["lastname"] },
                { "NSH_PASSWORD", account_["password"] },
                { "NSH_SHORTNAME", account_["shortname"] },
                { "NSH_INTERNETADDRESS", account_["internetaddress"] },
                { "NSH_JOB", job_ },
                { "NSH_COMMAND_ID", std::to_string(in.id) },
                { "NSH_SERVER", config_.serverUrl },
            };

            // The environment always carries everything. The arguments may use placeholders for all of it
            // except the password.
            EnvList values;

            for (const auto &kv : env)
            {
                if ("NSH_PASSWORD" != kv.first)
                    values.push_back(kv);
            }

            std::vector<std::string> args;
            std::string              err;
            bool                     expanded = true;

            for (const std::string &a : config_.programArgs)
            {
                std::string out;

                if (!ExpandArgTemplate(a, values, out, err))
                {
                    expanded = false;
                    break;
                }

                args.push_back(out);
            }

            if (expanded && child_.Start(config_.program, args, env, err))
            {
                state_   = "running";
                message_ = "running job " + job_;
            }
            else
            {
                state_   = "error";
                message_ = err;
                Say("error: " + err);
            }
        }
    }
    else if ("pause" == in.command)
    {
        if ("paused" != state_)
            resumeState_ = state_;

        state_      = "paused";
        pauseUntil_ = Clock::now() + std::chrono::seconds(in.pauseSeconds);
        message_    = "paused " + std::to_string(in.pauseSeconds) + "s";
    }
    else if ("stop" == in.command)
    {
        ReportFinal("stopping", "stopping");
        ReportFinal("done", "stopped by command");
        Say("done");
        return true;
    }

    Say("applied command " + std::to_string(in.id) + ": " + in.command + (job_.empty() || "run" != in.command ? "" : " " + job_));
    return false;
}

void LogicalClient::Interrupted()
{
    // SIGTERM, then SIGKILL if ignored, then reap: never leave a child behind
    child_.StopAndReap(5000);

    ReportFinal("done", "runner interrupted");
    Say("interrupted");
}

bool LogicalClient::Run()
{
    if (!Register())
    {
        // Interrupted before registering is not a failure; neither is finding the
        // pool fully booked in fill mode (that is how fill mode knows it is done).
        return Stopping() || (fill_ && REG_EXHAUSTED == regState_);
    }

    for (;;)
    {
        if (Stopping())
        {
            Interrupted();
            return true;
        }

        CheckChild();

        if ("paused" == state_ && Clock::now() >= pauseUntil_)
        {
            state_   = resumeState_;
            message_ = "pause over";
        }

        if ("running" == state_ && config_.program.empty())
            message_ = "dummy ops=" + std::to_string(++dummyOps_);

        bool ok = Report();

        if (lost_)
        {
            Say("coordinator no longer knows this client (restarted?)");
            return false;
        }

        // The error was just reported; the coordinator treats it as final
        if ("error" == state_)
            return false;

        if (ok && !child_.Running() && latest_.id > applied_)
        {
            if (Apply())
                return true;

            continue; // report the new state and acknowledgement right away
        }

        Sleep(config_.pollMs);
    }
}

} // namespace

bool ExpandArgTemplate(const std::string &arg, const EnvList &values, std::string &out, std::string &err)
{
    const char *nameChars = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_";
    size_t      pos       = 0;

    out.clear();

    while (pos < arg.size())
    {
        size_t open = arg.find("{NSH_", pos);

        if (std::string::npos == open)
        {
            out.append(arg, pos, std::string::npos);
            break;
        }

        size_t      close = arg.find('}', open);
        std::string name;

        if (std::string::npos != close)
            name = arg.substr(open + 1, close - open - 1);

        // Not a complete placeholder (no closing brace, or other characters inside): literal text
        if (name.empty() || std::string::npos != name.find_first_not_of(nameChars))
        {
            out.append(arg, pos, open + 1 - pos);
            pos = open + 1;
            continue;
        }

        out.append(arg, pos, open - pos);

        if ("NSH_PASSWORD" == name)
        {
            err = "{NSH_PASSWORD} is not allowed in program arguments (it would be visible in the process list); "
                  "read the environment variable NSH_PASSWORD instead";
            return false;
        }

        bool found = false;

        for (const auto &kv : values)
        {
            if (kv.first == name)
            {
                out += kv.second;
                found = true;
                break;
            }
        }

        if (!found)
        {
            err = "unknown placeholder {" + name + "}";
            return false;
        }

        pos = close + 1; // values are inserted as they are: no second pass over them
    }

    return true;
}

bool ValidateArgTemplates(const std::vector<std::string> &args, std::string &err)
{
    EnvList values;

    for (const char *name : { "NSH_TEST_ID", "NSH_FIRSTNAME", "NSH_LASTNAME", "NSH_SHORTNAME", "NSH_INTERNETADDRESS",
                              "NSH_JOB", "NSH_COMMAND_ID", "NSH_SERVER" })
    {
        values.push_back({ name, "x" });
    }

    for (const std::string &a : args)
    {
        std::string out;

        if (!ExpandArgTemplate(a, values, out, err))
            return false;
    }

    return true;
}

int RunRunner(const RunnerConfig &config, const std::atomic<bool> &stop)
{
    HttpUrl     url;
    std::string err;

    if (!ValidateArgTemplates(config.programArgs, err))
    {
        std::fprintf(stderr, "Invalid program argument: %s\n", err.c_str());
        return 1;
    }

    if (!ParseHttpUrl(config.serverUrl, url, err))
    {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }

    // Unique per runner start; combined with the client index it forms the request_key
    std::random_device rd;
    char               runId[24];
    std::snprintf(runId, sizeof(runId), "r%08x%08x", rd(), rd());

    // clients == 0: fill mode, start clients until the pool is booked or the limit is reached
    // (config.maxClients by default, MAX_CLIENTS with --clients all)
    const bool fill  = (config.clients <= 0);
    const int  limit = fill ? (config.fillAll ? MAX_CLIENTS : std::min(config.maxClients, MAX_CLIENTS)) : config.clients;

    if (fill)
        std::printf("Runner %s: up to %d clients, or until the account pool is booked -> %s, %s\n", runId, limit, config.serverUrl.c_str(),
                    config.program.empty() ? "built-in dummy job" : config.program.c_str());
    else
        std::printf("Runner %s: %d client(s) -> %s, %s\n", runId, limit, config.serverUrl.c_str(),
                    config.program.empty() ? "built-in dummy job" : config.program.c_str());

    std::fflush(stdout);

    std::vector<char>             failed(limit, 0); // not vector<bool>: threads write distinct elements
    std::vector<std::atomic<int>> regStates(limit);
    std::vector<std::thread>      threads;
    std::atomic<bool>             halt(false); // shuts every client down if we cannot start them all
    bool                          startFailed = false;

    for (int i = 0; i < limit && !stop; i++)
    {
        regStates[i] = REG_PENDING;

        try
        {
            threads.emplace_back([&, i]()
            {
                LogicalClient client(i + 1, config, url, runId, stop, halt, fill, regStates[i]);
                failed[i] = client.Run() ? 0 : 1;
            });
        }
        catch (const std::exception &e)
        {
            // Out of threads or memory: do not carry on with a partial herd. Stop the clients that
            // already run (they terminate their children and report done) and join them below.
            std::fprintf(stderr, "Cannot start client %d: %s; stopping the %zu client(s) already running\n", i + 1, e.what(), threads.size());
            halt        = true;
            startFailed = true;
            break;
        }

        if (!fill)
            continue;

        // Fill mode: one registration at a time, so we see exactly when the pool is booked
        while (REG_PENDING == regStates[i] && !stop)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));

        if (REG_OK != regStates[i])
            break;

        if (i + 1 == limit)
            std::printf("Client limit (%d) reached; the pool may have more free accounts (see --max-clients, --clients)\n", limit);
    }

    for (std::thread &t : threads)
        t.join();

    int started  = (int)threads.size();
    int failures = 0;
    int booked   = 0;

    for (int i = 0; i < started; i++)
    {
        failures += failed[i] ? 1 : 0;
        booked   += (REG_OK == regStates[i]) ? 1 : 0;
    }

    std::printf("Runner finished: %d client(s) registered, %d failed\n", booked, failures);
    return (failures || startFailed) ? 1 : 0;
}
