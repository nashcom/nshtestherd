// api.cpp - HTTP routing and request validation

#include "api.h"
#include "wire.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <initializer_list>

namespace
{

const char *CONTENT_TYPE_TEXT    = "text/plain; charset=utf-8";
const char *CONTENT_TYPE_JSON    = "application/json; charset=utf-8";
const char *CONTENT_TYPE_METRICS = "text/plain; version=0.0.4; charset=utf-8";

const size_t MAX_KEY_LENGTH     = 128;
const size_t MAX_JOB_LENGTH     = 128;
const size_t MAX_PARAMS_LENGTH  = 1024;
const long long MAX_PAUSE_SECONDS = 31536000; // one year

std::string Lower(std::string s)
{
    for (char &c : s)
        c = (char)std::tolower((unsigned char)c);

    return s;
}

bool WantsJson(const HttpRequest &req)
{
    return Lower(req.accept).find("application/json") != std::string::npos;
}

HttpResponse Reply(const HttpRequest &req, int status, const Fields &fields)
{
    HttpResponse r;
    r.status = status;

    if (WantsJson(req))
    {
        r.contentType = CONTENT_TYPE_JSON;
        r.body        = RenderJson(fields);
    }
    else
    {
        r.contentType = CONTENT_TYPE_TEXT;
        r.body        = RenderText(fields);
    }

    return r;
}

HttpResponse Fail(const HttpRequest &req, int status, const std::string &error, const std::string &message)
{
    Fields f;
    f.Add("error", error);
    f.Add("message", message);
    return Reply(req, status, f);
}

HttpResponse Fail(const HttpRequest &req, const Result &result)
{
    return Fail(req, result.code, result.error, result.message);
}

HttpResponse BadRequest(const HttpRequest &req, const std::string &message)
{
    return Fail(req, 400, "invalid_request", message);
}

void AddInstruction(Fields &f, const Instruction &i)
{
    f.Add("command", CommandName(i.kind));
    f.Add("command_id", i.commandId);
    f.Add("job", i.job);
    f.Add("params", i.params);
    f.Add("pause_seconds", i.pauseSeconds);
}

void AddAccount(Fields &f, const UserRecord &u)
{
    f.Add("firstname", u.firstName);
    f.Add("lastname", u.lastName);
    f.Add("password", u.password);
    f.Add("shortname", u.shortName);
    f.Add("internetaddress", u.internetAddress);
}

// Strict unsigned decimal in [min, max]
bool ParseInt(const std::string &s, long long min, long long max, long long &out)
{
    if (s.empty() || s.size() > 18)
        return false;

    long long n = 0;

    for (char c : s)
    {
        if (c < '0' || c > '9')
            return false;

        n = n * 10 + (c - '0');
    }

    if (n < min || n > max)
        return false;

    out = n;
    return true;
}

bool IsPrintableAscii(const std::string &s, bool allowSpace)
{
    for (char c : s)
    {
        unsigned char u = (unsigned char)c;

        if (u < 0x20 || u > 0x7E || (!allowSpace && ' ' == u))
            return false;
    }

    return true;
}

// Workload names are identifiers, not commands: keep them inert for shell workers.
bool IsValidJob(const std::string &job)
{
    if (job.empty() || job.size() > MAX_JOB_LENGTH)
        return false;

    for (char c : job)
    {
        if (!std::isalnum((unsigned char)c) && !std::strchr("._:/-", c))
            return false;
    }

    return true;
}

// Job parameters: the URL style options of the job ("name=value&name"), passed to the worker as they are. No blanks or
// control characters, so they stay one argument and one line everywhere.
bool IsValidParams(const std::string &params)
{
    return !params.empty() && params.size() <= MAX_PARAMS_LENGTH && IsPrintableAscii(params, false);
}

// "Authorization: Bearer <token>" with the right token. Compares in constant time, so the answer time does not tell how
// much of a guess was right.
bool TokenMatches(const std::string &authorization, const std::string &token)
{
    const std::string scheme = "bearer ";

    if (authorization.size() <= scheme.size() || Lower(authorization.substr(0, scheme.size())) != scheme)
        return false;

    const std::string given = authorization.substr(scheme.size());
    size_t            diff  = given.size() ^ token.size();

    for (size_t i = 0; i < token.size(); i++)
        diff |= (size_t)((unsigned char)token[i] ^ (unsigned char)(i < given.size() ? given[i] : 0));

    return 0 == diff;
}

// Rejects any field not in the allowed list.
bool CheckKeys(const Form &form, std::initializer_list<const char *> allowed, std::string &err)
{
    for (const auto &entry : form)
    {
        bool ok = false;

        for (const char *name : allowed)
        {
            if (entry.first == name)
                ok = true;
        }

        if (!ok)
        {
            err = "unknown field: " + entry.first;
            return false;
        }
    }

    return true;
}

// POST bodies: form-encoded; an empty body is an empty form.
bool ReadPostForm(const HttpRequest &req, Form &form, std::string &err)
{
    if (!req.body.empty())
    {
        std::string type = Lower(req.contentType.substr(0, req.contentType.find(';')));

        while (!type.empty() && (' ' == type.back() || '\t' == type.back()))
            type.pop_back();

        if ("application/x-www-form-urlencoded" != type)
        {
            err = "Content-Type must be application/x-www-form-urlencoded";
            return false;
        }
    }

    return ParseForm(req.body, form, err);
}

bool GetTestId(const Form &form, int &testId, std::string &err)
{
    auto it = form.find("test_id");

    if (it == form.end())
    {
        err = "test_id is required";
        return false;
    }

    long long n = 0;

    if (!ParseInt(it->second, 1, 2147483647, n))
    {
        err = "test_id must be a positive integer";
        return false;
    }

    testId = (int)n;
    return true;
}

HttpResponse HandleLoad(Herd &herd, const HttpRequest &req)
{
    std::string type = Lower(req.contentType.substr(0, req.contentType.find(';')));

    while (!type.empty() && (' ' == type.back() || '\t' == type.back()))
        type.pop_back();

    if ("text/csv" != type)
        return BadRequest(req, "Content-Type must be text/csv");

    size_t count = 0;
    Result r     = herd.LoadCsv(req.body, count);

    if (!r.Ok())
        return Fail(req, r);

    Fields f;
    f.Add("users", (long long)count);
    return Reply(req, 200, f);
}

HttpResponse HandleRegister(Herd &herd, const HttpRequest &req)
{
    Form        form;
    std::string err;

    if (!ReadPostForm(req, form, err) || !CheckKeys(form, { "request_key" }, err))
        return BadRequest(req, err);

    std::string key;
    auto        it = form.find("request_key");

    if (it != form.end())
    {
        key = it->second;

        if (key.empty() || key.size() > MAX_KEY_LENGTH || !IsPrintableAscii(key, false))
            return BadRequest(req, "request_key must be 1-128 printable ASCII characters without spaces");
    }

    bool       isNew = false;
    ClientView view;
    Result     r = herd.Register(key, isNew, view);

    if (!r.Ok())
        return Fail(req, r);

    Fields f;
    f.Add("test_id", (long long)view.testId);
    AddAccount(f, view.user);
    AddInstruction(f, view.instruction);
    f.Add("worker_options", herd.WorkerOptions());
    return Reply(req, isNew ? 201 : 200, f);
}

HttpResponse HandleStatusPost(Herd &herd, const HttpRequest &req)
{
    Form        form;
    std::string err;

    if (!ReadPostForm(req, form, err) || !CheckKeys(form, { "test_id", "state", "ack_command_id", "message", "last_job_id", "last_job", "last_job_result" }, err))
        return BadRequest(req, err);

    StatusReport report;

    if (!GetTestId(form, report.testId, err))
        return BadRequest(req, err);

    auto it = form.find("state");

    if (it == form.end() || !ParseWorkerState(it->second, report.state))
        return BadRequest(req, "state is required: idle, running, paused, stopping, done or error");

    it = form.find("ack_command_id");

    if (it != form.end())
    {
        if (!ParseInt(it->second, 0, 2147483647, report.ackCommandId))
            return BadRequest(req, "ack_command_id must be a non-negative integer");

        report.hasAck = true;
    }

    it = form.find("message");

    if (it != form.end())
    {
        if (it->second.size() > MAX_STATUS_MESSAGE_BYTES)
            return BadRequest(req, "message is longer than " + std::to_string(MAX_STATUS_MESSAGE_BYTES) + " bytes");

        report.hasMessage = true;
        report.message    = it->second;
    }

    // The last job that ended: all three fields or none
    size_t lastJobFields = form.count("last_job_id") + form.count("last_job") + form.count("last_job_result");

    if (0 != lastJobFields)
    {
        if (3 != lastJobFields)
            return BadRequest(req, "last_job_id, last_job and last_job_result go together");

        if (!ParseInt(form["last_job_id"], 1, 2147483647, report.lastJobId))
            return BadRequest(req, "last_job_id must be a positive integer");

        if (!IsValidJob(form["last_job"]))
            return BadRequest(req, "last_job must be 1-128 characters of A-Z a-z 0-9 . _ : / -");

        if (!ParseJobResult(form["last_job_result"], report.lastJobResult))
            return BadRequest(req, "last_job_result must be ok, failed or stopped");

        report.hasLastJob = true;
        report.lastJob    = form["last_job"];
    }

    ClientView view;
    Result     r = herd.ReportStatus(report, view);

    if (!r.Ok())
        return Fail(req, r);

    Fields f;
    f.Add("test_id", (long long)view.testId);
    AddInstruction(f, view.instruction);
    return Reply(req, 200, f);
}

HttpResponse HandleCommand(Herd &herd, const HttpRequest &req)
{
    Form        form;
    std::string err;

    if (!ReadPostForm(req, form, err) || !CheckKeys(form, { "target", "test_id", "command", "job", "params", "pause_seconds" }, err))
        return BadRequest(req, err);

    bool all = false;
    int  testId = 0;

    auto target = form.find("target");

    if (target != form.end())
    {
        if ("all" != target->second)
            return BadRequest(req, "target must be 'all'");

        if (form.count("test_id"))
            return BadRequest(req, "use either target=all or test_id, not both");

        all = true;
    }
    else if (!GetTestId(form, testId, err))
        return BadRequest(req, "test_id or target=all is required");

    Instruction command;
    auto        name = form.find("command");

    if (name == form.end() || !ParseCommandName(name->second, command.kind))
        return BadRequest(req, "command is required: idle, run, pause or stop");

    auto job    = form.find("job");
    auto params = form.find("params");
    auto pause  = form.find("pause_seconds");

    if (CommandKind::Run == command.kind)
    {
        if (job == form.end() || !IsValidJob(job->second))
            return BadRequest(req, "run requires job (1-128 characters of A-Z a-z 0-9 . _ : / -)");

        command.job = job->second;

        if (params != form.end())
        {
            if (!IsValidParams(params->second))
                return BadRequest(req, "params must be 1-1024 printable ASCII characters without blanks (name=value&name)");

            command.params = params->second;
        }
    }
    else if (job != form.end() || params != form.end())
        return BadRequest(req, "job and params are only valid with command=run");

    if (CommandKind::Pause == command.kind)
    {
        if (pause == form.end() || !ParseInt(pause->second, 1, MAX_PAUSE_SECONDS, command.pauseSeconds))
            return BadRequest(req, "pause requires pause_seconds between 1 and 31536000");
    }
    else if (pause != form.end())
        return BadRequest(req, "pause_seconds is only valid with command=pause");

    size_t    updated   = 0;
    long long commandId = 0;
    Result    r         = herd.SetCommand(all, testId, command, updated, commandId);

    if (!r.Ok())
        return Fail(req, r);

    Fields f;

    if (all)
        f.Add("target", "all");
    else
    {
        f.Add("test_id", (long long)testId);
        f.Add("command_id", commandId);
    }

    f.Add("command", CommandName(command.kind));
    f.Add("updated", (long long)updated);
    return Reply(req, 200, f);
}

HttpResponse HandleClientGet(Herd &herd, const HttpRequest &req)
{
    Form        form;
    std::string err;
    int         testId = 0;

    if (!ParseForm(req.query, form, err) || !CheckKeys(form, { "test_id" }, err) || !GetTestId(form, testId, err))
        return BadRequest(req, err);

    ClientView view;
    Result     r = herd.GetClient(testId, view);

    if (!r.Ok())
        return Fail(req, r);

    // Credentials are deliberately not part of this view
    Fields f;
    f.Add("test_id", (long long)view.testId);
    f.Add("shortname", view.user.shortName);
    f.Add("state", StateName(view.state));
    f.Add("ack_command_id", view.ackCommandId);
    f.Add("last_contact_seconds", view.ageSeconds);
    f.Add("message", view.message);
    AddInstruction(f, view.instruction);
    f.Add("last_job_id", view.lastJobId);
    f.Add("last_job", view.lastJob);
    f.Add("last_job_result", JobResultName(view.lastJobResult));
    f.Add("last_job_message", view.lastJobMessage);
    return Reply(req, 200, f);
}

HttpResponse HandleStatusGet(Herd &herd, const HttpRequest &req)
{
    Summary s = herd.GetSummary();

    Fields f;
    f.Add("users_total", (long long)s.usersTotal);
    f.Add("users_available", (long long)(s.usersTotal - s.usersAllocated));
    f.Add("users_allocated", (long long)s.usersAllocated);
    f.Add("clients_total", (long long)s.clientsTotal);

    for (int i = 0; i < STATE_COUNT; i++)
        f.Add(std::string("clients_") + StateName((ClientState)i), (long long)s.byState[i]);

    for (int i = 1; i < JOB_RESULT_COUNT; i++)
        f.Add(std::string("jobs_") + JobResultName((JobResult)i), (long long)s.jobsEnded[i]);

    f.Add("uptime_seconds", s.uptimeSeconds);
    return Reply(req, 200, f);
}

HttpResponse HandleHealth(const HttpRequest &req)
{
    Fields f;
    f.Add("status", "ok");
    return Reply(req, 200, f);
}

void MetricHeader(std::string &out, const char *name, const char *type, const char *help)
{
    out += std::string("# HELP nshtestherd_") + name + " " + help + "\n";
    out += std::string("# TYPE nshtestherd_") + name + " " + type + "\n";
}

void Metric(std::string &out, const char *name, const char *type, const char *help, unsigned long long value)
{
    MetricHeader(out, name, type, help);
    out += std::string("nshtestherd_") + name + " " + std::to_string(value) + "\n";
}

// Coordination metrics only. No ids, names, keys, messages or jobs as labels.
HttpResponse HandleMetrics(Herd &herd)
{
    Summary s = herd.GetSummary();
    std::string out;

    Metric(out, "users_total", "gauge", "Accounts in the loaded pool.", s.usersTotal);
    Metric(out, "users_available", "gauge", "Accounts not yet allocated.", s.usersTotal - s.usersAllocated);
    Metric(out, "users_allocated", "gauge", "Accounts allocated to clients.", s.usersAllocated);
    Metric(out, "clients_total", "gauge", "Registered clients.", s.clientsTotal);

    MetricHeader(out, "clients_by_state", "gauge", "Registered clients by last reported state.");

    for (int i = 0; i < STATE_COUNT; i++)
        out += std::string("nshtestherd_clients_by_state{state=\"") + StateName((ClientState)i) + "\"} " + std::to_string(s.byState[i]) + "\n";

    MetricHeader(out, "jobs_ended_total", "counter", "Jobs that ended, by result (counted once per job).");

    for (int i = 1; i < JOB_RESULT_COUNT; i++)
        out += std::string("nshtestherd_jobs_ended_total{result=\"") + JobResultName((JobResult)i) + "\"} " + std::to_string(s.jobsEnded[i]) + "\n";

    Metric(out, "uptime_seconds", "gauge", "Seconds since the coordinator started.", (unsigned long long)s.uptimeSeconds);
    Metric(out, "registrations_total", "counter", "New account allocations.", s.registrations);
    Metric(out, "allocation_failures_total", "counter", "Registrations rejected because no account was available.", s.allocationFailures);
    Metric(out, "status_reports_total", "counter", "Accepted worker status reports.", s.statusReports);
    Metric(out, "command_updates_total", "counter", "Command updates, counted per affected client.", s.commandUpdates);
    Metric(out, "csv_loads_total", "counter", "Successful CSV account imports.", s.csvLoads);
    Metric(out, "csv_load_failures_total", "counter", "Rejected CSV account imports.", s.csvLoadFailures);

    HttpResponse r;
    r.contentType = CONTENT_TYPE_METRICS;
    r.body        = out;
    return r;
}

} // namespace

HttpResponse HandleRequest(Herd &herd, const HttpRequest &req, const std::string &token)
{
    // With a token everything but the health check needs it, before anything else is looked at
    if (!token.empty() && "/health" != req.path && !TokenMatches(req.authorization, token))
    {
        HttpResponse r  = Fail(req, 401, "unauthorized", "missing or wrong token (Authorization: Bearer <token>)");
        r.authChallenge = true;
        return r;
    }

    struct Route
    {
        const char *path;
        const char *methods; // space separated
    };

    static const Route routes[] = {
        { "/load", "POST" },
        { "/register", "POST" },
        { "/status", "GET POST" },
        { "/command", "POST" },
        { "/client", "GET" },
        { "/metrics", "GET" },
        { "/health", "GET" },
    };

    const Route *route = NULL;

    for (const Route &candidate : routes)
    {
        if (req.path == candidate.path)
            route = &candidate;
    }

    if (!route)
        return Fail(req, 404, "not_found", "unknown path");

    if ((std::string(" ") + route->methods + " ").find(" " + req.method + " ") == std::string::npos)
    {
        HttpResponse r = Fail(req, 405, "method_not_allowed", "method not allowed for this path");
        r.allow        = route->methods;
        std::replace(r.allow.begin(), r.allow.end(), ' ', ',');
        return r;
    }

    const std::string &p = req.path;
    const bool         get = ("GET" == req.method);

    if ("/load" == p)
        return HandleLoad(herd, req);

    if ("/register" == p)
        return HandleRegister(herd, req);

    if ("/status" == p)
        return get ? HandleStatusGet(herd, req) : HandleStatusPost(herd, req);

    if ("/command" == p)
        return HandleCommand(herd, req);

    if ("/client" == p)
        return HandleClientGet(herd, req);

    if ("/metrics" == p)
        return HandleMetrics(herd);

    return HandleHealth(req);
}
