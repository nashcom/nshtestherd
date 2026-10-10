// herdclient.cpp - the worker side of the nshtestherd protocol

#include "herdclient.h"

#include "wire.h"

#include <chrono>
#include <cstdlib>
#include <thread>

namespace
{

long long NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool IsNumber(const std::string &s)
{
    return !s.empty() && s.size() <= 15 && std::string::npos == s.find_first_not_of("0123456789");
}

// The coordinator refuses a status report with a message over MAX_STATUS_MESSAGE_BYTES (wire.h). A refused report would
// be sent again on every poll, and the client would never see a new command: cut it instead, not inside a UTF-8 character.
std::string LimitMessage(const std::string &message)
{
    if (message.size() <= MAX_STATUS_MESSAGE_BYTES)
        return message;

    size_t length = MAX_STATUS_MESSAGE_BYTES;

    // The first byte that is cut off is a continuation byte: the character it belongs to goes as a whole
    while (length > 0 && 0x80 == ((unsigned char)message[length] & 0xC0))
        length--;

    return message.substr(0, length);
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

} // namespace

HerdClient::HerdClient(const HerdClientConfig &config, HerdHooks &hooks) : config_(config), hooks_(hooks)
{
}

HerdRegistration HerdClient::Register()
{
    if (config_.startDelayMs > 0)
        hooks_.Sleep(config_.startDelayMs);

    std::string lastReason;   // the log gets a line when the reason changes, not on every attempt

    while (!hooks_.Stopping())
    {
        HttpReply   reply;
        std::string err;
        std::string reason;

        if (HttpCall(url_, "POST", "/register", "request_key=" + UrlEncode(config_.requestKey), reply, err, config_.httpTimeoutSeconds, config_.token))
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
                    account_.testId          = f["test_id"];
                    account_.firstName       = f["firstname"];
                    account_.lastName        = f["lastname"];
                    account_.password        = f["password"];
                    account_.shortName       = f["shortname"];
                    account_.internetAddress = f["internetaddress"];
                    account_.workerOptions   = f["worker_options"];   // optional: older coordinators do not send it
                    Say("registered as " + account_.shortName);
                    return HERD_REG_OK;
                }
            }
            // The pool may simply not be loaded yet; anything else is final
            else if (409 == reply.status && "no_accounts_loaded" == f["error"])
                reason = "no account pool loaded yet";
            else if (409 == reply.status && "pool_exhausted" == f["error"])
            {
                if (!config_.waitForAccount)
                {
                    Say("no free account left");
                    return HERD_REG_EXHAUSTED;
                }

                reason = "no free account left";
            }
            else if (503 != reply.status)
            {
                Say("registration refused: " + f["message"]);
                return HERD_REG_FAILED;
            }
        }
        else
            reason = "coordinator unreachable: " + err;

        if (!reason.empty())
        {
            if (reason != lastReason)
                Say("waiting: " + reason);

            lastReason = reason;
            hooks_.Waiting(reason);
        }

        hooks_.Sleep(config_.pollMs);
    }

    return HERD_REG_FAILED;
}

bool HerdClient::Report()
{
    std::string body = "test_id=" + account_.testId + "&state=" + state_ + "&ack_command_id=" + std::to_string(applied_) + "&message=" + UrlEncode(LimitMessage(message_));

    // The last job that ended goes with every report: the coordinator counts it once, by its id
    if (lastJobId_ > 0)
        body += "&last_job_id=" + std::to_string(lastJobId_) + "&last_job=" + UrlEncode(lastJob_) + "&last_job_result=" + lastJobResult_;

    HttpReply   reply;
    std::string err;

    if (!HttpCall(url_, "POST", "/status", body, reply, err, config_.httpTimeoutSeconds, config_.token))
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

    // Accepted: the coordinator has counted the last job that went with this report
    if (lastJobId_ > 0)
        lastJobReported_ = true;

    // A 200 without the instruction fields is a damaged reply: do not act on defaults
    if (!f.count("command_id") || !f.count("command") || !f.count("job") || !f.count("pause_seconds") || !IsNumber(f["command_id"]) || !IsNumber(f["pause_seconds"]))
    {
        Say("incomplete status reply; ignored");
        return false;
    }

    latest_.id           = std::atoll(f["command_id"].c_str());
    latest_.command      = f["command"];
    latest_.job          = f["job"];
    latest_.params       = f["params"];   // optional: a coordinator without job parameters does not send it
    latest_.pauseSeconds = std::atoll(f["pause_seconds"].c_str());
    return true;
}

bool HerdClient::ReportFinal(const std::string &state, const std::string &message)
{
    SetState(state, message);

    for (int attempt = 0; attempt < 3; attempt++)
    {
        if (Report())
            return true;

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    return false;
}

void HerdClient::SetState(const std::string &state, const std::string &message)
{
    bool changed = (state != state_) || (message != message_);

    state_   = state;
    message_ = message;

    if (changed)
        hooks_.StateChanged(state_, message_);
}

// Only one result goes with a report. A result that the coordinator has not accepted yet would be lost when the next
// job ends before the next successful report (for example "stopped" by a new run command that then fails to start).
void HerdClient::DeliverLastJob()
{
    for (int attempt = 0; attempt < 3 && !lastJobReported_ && !lost_; attempt++)
    {
        if (attempt > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(200));

        Report();
    }

    if (!lastJobReported_)
        Say("result of job " + lastJob_ + " (" + lastJobResult_ + ") not delivered: coordinator unreachable");
}

void HerdClient::EndJob(const std::string &result, const std::string &message)
{
    DeliverLastJob();

    jobActive_       = false;
    lastJobId_       = jobId_;
    lastJob_         = job_;
    lastJobResult_   = result;
    lastJobReported_ = false;
    resumeState_     = "idle";

    SetState("idle", message);
    Say("job " + job_ + " " + result + (message.empty() ? "" : ": " + message));
}

void HerdClient::StopJob(bool reportNow)
{
    if (!jobActive_)
        return;

    // The program decides how to stop its job (the runner: SIGTERM, then SIGKILL, then reap)
    hooks_.JobAbort();
    EndJob("stopped", "job " + job_ + " stopped");

    // Reported on its own, so the result keeps its own message and is not mixed up with what comes next
    if (reportNow)
        Report();
}

// Called only when no job is busy. Each command_id is applied exactly once.
bool HerdClient::Apply()
{
    const Instruction in = latest_;

    // A new idle, run or stop command ends the job that is still there; a pause keeps it
    if ("pause" != in.command)
        StopJob(true);

    applied_ = in.id;
    Say("applying command " + std::to_string(in.id) + ": " + in.command + (in.job.empty() ? "" : " " + in.job));

    if ("idle" == in.command)
        SetState("idle", "waiting for work");
    else if ("run" == in.command)
    {
        HerdJobContext context;
        std::string    message;
        std::string    err;

        context.account    = account_;
        context.job        = in.job;
        context.params     = in.params;
        context.commandId  = in.id;
        context.serverUrl  = config_.serverUrl;

        jobId_ = in.id;
        job_   = in.job;

        if (hooks_.JobStart(context, message, err))
        {
            jobActive_ = true;
            SetState("running", message);
        }
        else
            EndJob("failed", err);   // the client stays and waits for the next command
    }
    else if ("pause" == in.command)
    {
        if ("paused" != state_)
            resumeState_ = state_;

        pauseEndMs_ = NowMs() + in.pauseSeconds * 1000;
        SetState("paused", "paused " + std::to_string(in.pauseSeconds) + "s");
    }
    else if ("stop" == in.command)
    {
        ReportFinal("stopping", "stopping");
        ReportFinal("done", "stopped by command");
        Say("done");
        return true;
    }

    return false;
}

void HerdClient::Interrupted()
{
    // No report of its own: the final "done" carries the result, and a shutdown should not wait for an extra call
    StopJob(false);

    ReportFinal("done", config_.name + " interrupted");
    Say("interrupted");
}

HerdEnd HerdClient::Run()
{
    std::string err;

    if (!ParseHttpUrl(config_.serverUrl, url_, err))
    {
        Say("invalid coordinator URL: " + err);
        hooks_.RegistrationDone(HERD_REG_FAILED);
        return HERD_END_FAILED;
    }

    HerdRegistration reg = Register();
    hooks_.RegistrationDone(reg);

    if (HERD_REG_OK != reg)
    {
        // Interrupted before registering is not a failure
        if (hooks_.Stopping())
            return HERD_END_CLEAN;

        return (HERD_REG_EXHAUSTED == reg) ? HERD_END_POOL_EXHAUSTED : HERD_END_FAILED;
    }

    // The only "error": a client that cannot work at all. A job that fails is reported as such, the client stays.
    if (!hooks_.Registered(account_, err))
    {
        Say("identity setup failed: " + err);
        ReportFinal("error", "identity setup failed: " + err);
        return HERD_END_FAILED;
    }

    SetState("idle", "waiting for work");

    for (;;)
    {
        if (hooks_.Stopping())
        {
            Interrupted();
            return HERD_END_CLEAN;
        }

        // A timed pause ends on the client's own monotonic clock: back to the state before the pause
        if ("paused" == state_ && NowMs() >= pauseEndMs_)
            SetState(resumeState_, "pause over");

        if ("running" == state_)
        {
            std::string message = message_;

            switch (hooks_.JobStep(message))
            {
                case HERD_JOB_FINISHED:
                    EndJob("ok", message);
                    break;

                case HERD_JOB_FAILED:
                    EndJob("failed", message);
                    break;

                default:
                    SetState(state_, message);
                    break;
            }
        }

        bool ok = Report();

        if (lost_)
        {
            Say("coordinator no longer knows this client (restarted?)");
            return HERD_END_FAILED;
        }

        if (ok && !hooks_.JobBusy() && latest_.id > applied_)
        {
            if (Apply())
                return HERD_END_CLEAN;

            continue; // report the new state and acknowledgement right away
        }

        hooks_.Sleep(config_.pollMs);
    }
}
