// runner.cpp - optional runner: logical clients (one thread each) using the HTTP API
//
// The protocol logic (registration, polling, commands, pause timer) is the shared HerdClient in herdclient.cpp,
// which domlem uses as well. This file supplies what is specific to the generic runner: a thread per client,
// child processes (or the built-in dummy job) as the job, and the console log.

#include "runner.h"

#include "herdclient.h"
#include "httpclient.h"
#include "process.h"

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

std::mutex g_logMutex;

void PrintLog(int index, const std::string &testId, const std::string &text)
{
    std::lock_guard<std::mutex> lock(g_logMutex);
    std::printf("[client %d%s%s] %s\n", index, testId.empty() ? "" : " id ", testId.c_str(), text.c_str());
    std::fflush(stdout);
}

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

// What is specific to the generic runner: how to wait, how to log, and what a "run" command does
// (start a child process, or the built-in dummy job when no program is given).
class RunnerHooks : public HerdHooks
{
public:
    // stop: the caller's interrupt flag; halt: set by the runner itself to shut every client down
    // (for example when a thread could not be started)
    RunnerHooks(int index, const RunnerConfig &config, const std::atomic<bool> &stop, const std::atomic<bool> &halt, std::atomic<int> &regState)
        : index_(index), config_(config), stop_(stop), halt_(halt), regState_(regState)
    {
    }

    bool Stopping() override
    {
        return stop_ || halt_;
    }

    void Sleep(int ms) override
    {
        // Sliced so a stop request is noticed quickly
        for (int waited = 0; waited < ms && !Stopping(); waited += 50)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    void Log(const std::string &testId, const std::string &text) override
    {
        PrintLog(index_, testId, text);
    }

    void RegistrationDone(HerdRegistration outcome) override
    {
        regState_ = (HERD_REG_OK == outcome) ? REG_OK : (HERD_REG_EXHAUSTED == outcome) ? REG_EXHAUSTED : REG_FAILED;
    }

    bool          JobStart(const HerdJobContext &context, std::string &message, std::string &err) override;
    bool          JobBusy() override { return child_.Running(); }
    HerdJobResult JobStep(std::string &message) override;

    void JobAbort() override
    {
        // SIGTERM, then SIGKILL if ignored, then reap: never leave a child behind
        child_.StopAndReap(5000);
    }

private:
    int                      index_;
    const RunnerConfig      &config_;
    const std::atomic<bool> &stop_;
    const std::atomic<bool> &halt_;
    std::atomic<int>        &regState_;

    std::string  job_;
    long long    dummyOps_ = 0;
    ChildProcess child_;
};

bool RunnerHooks::JobStart(const HerdJobContext &context, std::string &message, std::string &err)
{
    job_ = context.job;

    if (config_.program.empty())
    {
        message = "dummy job " + job_;
        return true;
    }

    const HerdAccount &a = context.account;

    EnvList env = {
        { "NSH_TEST_ID", a.testId },
        { "NSH_FIRSTNAME", a.firstName },
        { "NSH_LASTNAME", a.lastName },
        { "NSH_PASSWORD", a.password },
        { "NSH_SHORTNAME", a.shortName },
        { "NSH_INTERNETADDRESS", a.internetAddress },
        { "NSH_JOB", context.job },
        { "NSH_COMMAND_ID", std::to_string(context.commandId) },
        { "NSH_SERVER", context.serverUrl },
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

    for (const std::string &arg : config_.programArgs)
    {
        std::string out;

        if (!ExpandArgTemplate(arg, values, out, err))
            return false;

        args.push_back(out);
    }

    if (!child_.Start(config_.program, args, env, err))
        return false;

    message = "running job " + job_;
    return true;
}

HerdJobResult RunnerHooks::JobStep(std::string &message)
{
    if (config_.program.empty())
    {
        message = "dummy ops=" + std::to_string(++dummyOps_);
        return HERD_JOB_RUNNING;
    }

    int code = 0;

    if (!child_.Finished(code))
        return HERD_JOB_RUNNING;

    if (0 == code)
    {
        message = "job " + job_ + " finished (exit 0)";
        return HERD_JOB_FINISHED;
    }

    message = "job " + job_ + " failed (exit " + std::to_string(code) + ")";
    return HERD_JOB_FAILED;
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
                RunnerHooks hooks(i + 1, config, stop, halt, regStates[i]);

                HerdClientConfig clientConfig;
                clientConfig.serverUrl    = config.serverUrl;
                clientConfig.requestKey   = std::string(runId) + "-" + std::to_string(i + 1);
                clientConfig.name         = "runner";
                clientConfig.pollMs       = config.pollMs;
                // Spread start-up so many clients do not hit the coordinator in one burst
                // (fill mode registers one client at a time already)
                clientConfig.startDelayMs = fill ? 0 : ((i + 1) % 200) * 5;
                clientConfig.waitForAccount = false;   // the runner starts what the pool has: an exhausted pool ends the client

                HerdClient client(clientConfig, hooks);
                HerdEnd    end = client.Run();

                // A booked pool ends a client cleanly only in fill mode: that is how fill mode knows it is done
                bool ok = (HERD_END_CLEAN == end) || (fill && HERD_END_POOL_EXHAUSTED == end);
                failed[i] = ok ? 0 : 1;
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
