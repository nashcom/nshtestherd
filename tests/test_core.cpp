// test_core.cpp - focused checks without a test framework (no sockets involved)
//
// Covers: CSV quoting/rollback, wire escaping and form decoding, allocation
// and retry keys, exhaustion, concurrency, command/state separation, metrics.

#include "../src/api.h"
#include "../src/csv.h"
#include "../src/wire.h"

#include <atomic>
#include <cstdio>
#include <set>
#include <sstream>
#include <thread>

static int g_failures = 0;
static int g_checks   = 0;

#define CHECK(cond)                                                                    \
    do                                                                                 \
    {                                                                                  \
        g_checks++;                                                                    \
        if (!(cond))                                                                   \
        {                                                                              \
            g_failures++;                                                              \
            std::printf("[ FAIL ] %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
        }                                                                              \
    } while (0)

// ---- helpers ----------------------------------------------------------------

static HttpResponse Call(Herd &herd, const std::string &method, const std::string &target, const std::string &body = "", const std::string &accept = "")
{
    HttpRequest req;
    req.method = method;
    size_t q   = target.find('?');
    req.path   = target.substr(0, q);

    if (std::string::npos != q)
        req.query = target.substr(q + 1);

    req.body   = body;
    req.accept = accept;

    if ("/load" == req.path)
        req.contentType = "text/csv";
    else if (!body.empty())
        req.contentType = "application/x-www-form-urlencoded";

    return HandleRequest(herd, req);
}

// Parses the sectionless text format (single-line values only in these tests)
static Form Parse(const HttpResponse &r)
{
    Form              f;
    std::istringstream in(r.body);
    std::string        line;

    while (std::getline(in, line))
    {
        size_t eq = line.find('=');

        if (std::string::npos != eq)
            f[line.substr(0, eq)] = line.substr(eq + 1);
    }

    return f;
}

static std::string MakeCsv(int count)
{
    std::string csv;

    for (int i = 1; i <= count; i++)
    {
        char line[160];
        std::snprintf(line, sizeof(line), "Load,%06d,Secret%d,load%06d,load%06d@example.com\n", i, i, i, i);
        csv += line;
    }

    return csv;
}

static void Load(Herd &herd, int count)
{
    HttpResponse r = Call(herd, "POST", "/load", MakeCsv(count));
    CHECK(200 == r.status);
}

// ---- tests ------------------------------------------------------------------

static void TestCsv()
{
    std::vector<UserRecord> u;
    std::string             err;

    // headerless, CRLF, BOM-less
    CHECK(ParseUsersCsv("A,B,pw,a1,a1@x\r\nC,D,pw2,a2,a2@x\r\n", u, err));
    CHECK(2 == u.size() && "a2" == u[1].shortName && "a2@x" == u[1].internetAddress);

    // header row (case-insensitive) is skipped, BOM stripped, blank lines skipped
    CHECK(ParseUsersCsv("\xEF\xBB\xBF" "firstname,LastName,Password,Shortname,InternetAddress\n\nA,B,pw,a1,m\n\n", u, err));
    CHECK(1 == u.size() && "A" == u[0].firstName);

    // quoting: comma inside, escaped quote, embedded newline, empty fields, preserved spaces
    CHECK(ParseUsersCsv("\"Smith, J\",\"Say \"\"hi\"\"\",\" pw with spaces \",s1,\n,,,s2,x\n\"a\nb\",l,p,s3,m", u, err));
    CHECK(3 == u.size());
    CHECK("Smith, J" == u[0].firstName);
    CHECK("Say \"hi\"" == u[0].lastName);
    CHECK(" pw with spaces " == u[0].password);
    CHECK(u[0].internetAddress.empty());
    CHECK(u[1].firstName.empty() && u[1].password.empty());
    CHECK("a\nb" == u[2].firstName);

    // errors carry the record number
    CHECK(!ParseUsersCsv("A,B,pw,a1,m\nA,B,pw,a2\n", u, err) && err.find("record 2") == 0 && err.find("5 columns") != std::string::npos);
    CHECK(!ParseUsersCsv("A,B,pw,,m\n", u, err) && err.find("record 1") == 0 && err.find("Shortname") != std::string::npos);
    CHECK(!ParseUsersCsv("A,B,pw,a1,m\nA,B,pw,a1,m\n", u, err) && err.find("record 2") == 0 && err.find("duplicate") != std::string::npos);
    CHECK(!ParseUsersCsv("A,B,\"pw,a1,m\n", u, err) && err.find("unterminated") != std::string::npos);
    CHECK(!ParseUsersCsv("A,B,p\"w,a1,m\n", u, err));
    CHECK(!ParseUsersCsv("A,B,\"pw\"x,a1,m\n", u, err));
    CHECK(!ParseUsersCsv("", u, err) && u.empty());
    CHECK(!ParseUsersCsv("\n\n", u, err));
}

static void TestWire()
{
    CHECK("a\\\\b\\nc\\rd\\te=f" == EscapeText("a\\b\nc\rd\te=f"));

    Fields f;
    f.Add("k", "v=1 2");
    f.Add("n", 5);
    CHECK("k=v=1 2\nn=5\n" == RenderText(f));

    CHECK("{\"k\":\"v=1 2\",\"n\":5}\n" == RenderJson(f));
    CHECK("\\\"\\\\\\n\\u0001" == EscapeJson("\"\\\n\x01"));
    CHECK("caf\xC3\xA9" == EscapeJson("caf\xC3\xA9"));
    CHECK("\\ufffd" == EscapeJson("\xFF"));

    Form        form;
    std::string err;
    CHECK(ParseForm("a=1%2B2&b=x+y&c=%C3%A9&d=", form, err));
    CHECK("1+2" == form["a"] && "x y" == form["b"] && "\xC3\xA9" == form["c"] && form["d"].empty());
    CHECK(ParseForm("", form, err) && form.empty());
    CHECK(!ParseForm("a=%zz", form, err));
    CHECK(!ParseForm("a=%4", form, err));
    CHECK(!ParseForm("a=1&a=2", form, err));
    CHECK(!ParseForm("novalue", form, err));
    CHECK(!ParseForm("a=%00", form, err));
    CHECK(!ParseForm("=1", form, err));
}

static void TestLoadRollbackAndPolicy()
{
    Herd herd;

    // registration fails cleanly before any load
    HttpResponse r = Call(herd, "POST", "/register");
    CHECK(409 == r.status && "no_accounts_loaded" == Parse(r)["error"]);

    Load(herd, 3);

    // malformed import leaves the pool intact
    r = Call(herd, "POST", "/load", "A,B,p,x1,m\nbroken\n");
    CHECK(400 == r.status && Parse(r)["message"].find("record 2") == 0);
    CHECK("3" == Parse(Call(herd, "GET", "/status"))["users_total"]);

    // wrong content type
    HttpRequest bad;
    bad.method      = "POST";
    bad.path        = "/load";
    bad.contentType = "application/json";
    bad.body        = MakeCsv(1);
    CHECK(400 == HandleRequest(herd, bad).status);

    // replace is allowed until the first registration, then 409
    Load(herd, 5);
    CHECK("5" == Parse(Call(herd, "GET", "/status"))["users_total"]);
    CHECK(201 == Call(herd, "POST", "/register").status);
    r = Call(herd, "POST", "/load", MakeCsv(2));
    CHECK(409 == r.status);
    CHECK("5" == Parse(Call(herd, "GET", "/status"))["users_total"]);
}

static void TestRegistration()
{
    Herd herd;
    Load(herd, 3);

    HttpResponse r1 = Call(herd, "POST", "/register", "request_key=pod-a");
    Form         f1 = Parse(r1);
    CHECK(201 == r1.status && "1" == f1["test_id"] && "load000001" == f1["shortname"] && "Secret1" == f1["password"]);
    CHECK("idle" == f1["command"] && "0" == f1["command_id"]);

    // retry with the same key returns the same allocation
    HttpResponse r1b = Call(herd, "POST", "/register", "request_key=pod-a");
    CHECK(200 == r1b.status && "1" == Parse(r1b)["test_id"] && "load000001" == Parse(r1b)["shortname"]);

    // failed requests do not consume numbers
    CHECK(400 == Call(herd, "POST", "/register", "request_key=%20bad").status);
    CHECK(400 == Call(herd, "POST", "/register", "bogus=1").status);
    CHECK(400 == Call(herd, "POST", "/register", "request_key=a&request_key=b").status);

    Form f2 = Parse(Call(herd, "POST", "/register"));
    Form f3 = Parse(Call(herd, "POST", "/register", "request_key=pod-c"));
    CHECK("2" == f2["test_id"] && "load000002" == f2["shortname"]);
    CHECK("3" == f3["test_id"] && "load000003" == f3["shortname"]);

    // exhaustion
    HttpResponse r4 = Call(herd, "POST", "/register");
    CHECK(409 == r4.status && "pool_exhausted" == Parse(r4)["error"]);

    // a retry with a known key still works when the pool is exhausted
    CHECK(200 == Call(herd, "POST", "/register", "request_key=pod-c").status);

    // JSON response
    HttpResponse j = Call(herd, "POST", "/register", "request_key=pod-a", "application/json");
    CHECK(j.contentType.find("application/json") == 0 && j.body.find("\"test_id\":1,") != std::string::npos);
    CHECK(j.body.find("\"shortname\":\"load000001\"") != std::string::npos);

    // errors in JSON
    CHECK(Call(herd, "POST", "/register", "", "application/json").body.find("{\"error\":\"pool_exhausted\"") == 0);
}

static void TestConcurrentAllocation()
{
    const int threads = 16;
    const int perThread = 50;
    const int pool = 400; // 800 attempts, 400 must succeed

    Herd herd;
    Load(herd, pool);

    std::mutex            m;
    std::set<std::string> ids;
    std::set<std::string> names;
    std::atomic<int>      ok(0);
    std::atomic<int>      exhausted(0);
    std::vector<std::thread> pt;

    for (int t = 0; t < threads; t++)
    {
        pt.emplace_back([&]()
        {
            for (int i = 0; i < perThread; i++)
            {
                HttpResponse r = Call(herd, "POST", "/register");
                Form         f = Parse(r);

                if (201 == r.status)
                {
                    ok++;
                    std::lock_guard<std::mutex> lock(m);
                    ids.insert(f["test_id"]);
                    names.insert(f["shortname"]);
                }
                else if (409 == r.status)
                    exhausted++;
            }
        });
    }

    for (std::thread &t : pt)
        t.join();

    CHECK(pool == ok);
    CHECK(threads * perThread - pool == exhausted);
    CHECK((size_t)pool == ids.size());
    CHECK((size_t)pool == names.size());
    CHECK(ids.count("1") && ids.count("400") && !ids.count("401"));
}

static void TestCommandsAndState()
{
    Herd herd;
    Load(herd, 3);

    Call(herd, "POST", "/register");
    Call(herd, "POST", "/register");

    // validation
    CHECK(400 == Call(herd, "POST", "/command", "test_id=1&command=run").status);
    CHECK(400 == Call(herd, "POST", "/command", "test_id=1&command=run&job=a%20b").status);
    CHECK(400 == Call(herd, "POST", "/command", "test_id=1&command=pause").status);
    CHECK(400 == Call(herd, "POST", "/command", "test_id=1&command=pause&pause_seconds=0").status);
    CHECK(400 == Call(herd, "POST", "/command", "test_id=1&command=stop&job=x").status);
    CHECK(400 == Call(herd, "POST", "/command", "test_id=1&command=idle&pause_seconds=5").status);
    CHECK(400 == Call(herd, "POST", "/command", "test_id=1&command=jump").status);
    CHECK(400 == Call(herd, "POST", "/command", "command=stop").status);
    CHECK(400 == Call(herd, "POST", "/command", "target=all&test_id=1&command=stop").status);
    CHECK(400 == Call(herd, "POST", "/command", "target=some&command=stop").status);
    CHECK(404 == Call(herd, "POST", "/command", "test_id=99&command=stop").status);

    // single target: ids increment per accepted update only
    Form c1 = Parse(Call(herd, "POST", "/command", "test_id=1&command=run&job=mail-read"));
    CHECK("1" == c1["command_id"] && "1" == c1["updated"]);
    Form c2 = Parse(Call(herd, "POST", "/command", "test_id=1&command=pause&pause_seconds=30"));
    CHECK("2" == c2["command_id"]);

    // repeated polls return the same instruction and id (no restart on the worker)
    for (int i = 0; i < 3; i++)
    {
        Form s = Parse(Call(herd, "POST", "/status", "test_id=1&state=running&ack_command_id=1"));
        CHECK("pause" == s["command"] && "2" == s["command_id"] && "30" == s["pause_seconds"] && s["job"].empty());
    }

    // desired and reported state are separate; ack tracks independently
    Form cl = Parse(Call(herd, "GET", "/client?test_id=1"));
    CHECK("running" == cl["state"] && "1" == cl["ack_command_id"] && "pause" == cl["command"] && "2" == cl["command_id"]);
    Call(herd, "POST", "/status", "test_id=1&state=running&ack_command_id=2&message=resumed");
    cl = Parse(Call(herd, "GET", "/client?test_id=1"));
    CHECK("running" == cl["state"] && "2" == cl["ack_command_id"] && "pause" == cl["command"] && "resumed" == cl["message"]);
    CHECK(cl.find("password") == cl.end());

    // ack beyond the issued id, unknown id, bad state
    CHECK(400 == Call(herd, "POST", "/status", "test_id=1&state=running&ack_command_id=9").status);
    CHECK(404 == Call(herd, "POST", "/status", "test_id=42&state=idle").status);
    CHECK(400 == Call(herd, "POST", "/status", "test_id=1&state=registered").status);
    CHECK(400 == Call(herd, "POST", "/status", "test_id=1").status);

    // fan-out hits the clients that exist now and bumps each id once
    Form all = Parse(Call(herd, "POST", "/command", "target=all&command=stop"));
    CHECK("2" == all["updated"]);
    CHECK("3" == Parse(Call(herd, "GET", "/client?test_id=1"))["command_id"]);
    CHECK("1" == Parse(Call(herd, "GET", "/client?test_id=2"))["command_id"]);

    // clients registered later start idle
    Form late = Parse(Call(herd, "POST", "/register"));
    CHECK("idle" == late["command"] && "0" == late["command_id"]);

    // repeating an admin submission is a new id
    CHECK("4" == Parse(Call(herd, "POST", "/command", "test_id=1&command=stop"))["command_id"]);

    // terminal clients cannot be restarted and are skipped by fan-out
    Call(herd, "POST", "/status", "test_id=1&state=done&ack_command_id=4");
    CHECK(409 == Call(herd, "POST", "/command", "test_id=1&command=run&job=x").status);
    Form all2 = Parse(Call(herd, "POST", "/command", "target=all&command=idle"));
    CHECK("2" == all2["updated"]); // clients 2 and 3
    CHECK("4" == Parse(Call(herd, "GET", "/client?test_id=1"))["command_id"]);

    // a finished client stays finished
    Call(herd, "POST", "/status", "test_id=1&state=running");
    CHECK("done" == Parse(Call(herd, "GET", "/client?test_id=1"))["state"]);

    // method / path handling
    HttpResponse m = Call(herd, "GET", "/register");
    CHECK(405 == m.status && "POST" == m.allow);
    CHECK(404 == Call(herd, "GET", "/nope").status);
    CHECK(404 == Call(herd, "GET", "/client?test_id=77").status);
    CHECK(400 == Call(herd, "GET", "/client").status);
    CHECK("ok" == Parse(Call(herd, "GET", "/health"))["status"]);
}

// With a token every call but the health check needs "Authorization: Bearer <token>"
static void TestToken()
{
    Herd              herd;
    const std::string token = "s3cret-token-0123456789";
    Load(herd, 2);

    auto call = [&](const std::string &method, const std::string &path, const std::string &authorization)
    {
        HttpRequest req;
        req.method        = method;
        req.path          = path;
        req.authorization = authorization;
        return HandleRequest(herd, req, token);
    };

    HttpResponse none = call("GET", "/status", "");
    CHECK(401 == none.status && none.authChallenge && "unauthorized" == Parse(none)["error"]);
    CHECK(401 == call("GET", "/status", "Bearer wrong-token-0123456789").status);
    CHECK(401 == call("GET", "/status", "Bearer " + token.substr(0, token.size() - 1)).status);   // a prefix is not enough
    CHECK(401 == call("GET", "/status", "Bearer " + token + "x").status);
    CHECK(401 == call("GET", "/status", "Basic " + token).status);
    CHECK(401 == call("POST", "/register", "").status);
    CHECK(401 == call("GET", "/nope", "").status);                                              // no path probing either

    CHECK(200 == call("GET", "/status", "Bearer " + token).status);
    CHECK(200 == call("GET", "/status", "bearer " + token).status);                              // the scheme is case insensitive
    CHECK(201 == call("POST", "/register", "Bearer " + token).status);
    CHECK(200 == call("GET", "/health", "").status);                                             // liveness stays open

    // without a token configured nothing changes
    CHECK(200 == Call(herd, "GET", "/status").status);
}

// The coordinator's worker options come back with every registration, unchanged (empty when there are none)
static void TestWorkerOptions()
{
    Herd herd;
    Load(herd, 3);

    Form r = Parse(Call(herd, "POST", "/register"));
    CHECK(r.count("worker_options") && r["worker_options"].empty());

    herd.SetWorkerOptions("switch&kind=domino");
    r = Parse(Call(herd, "POST", "/register"));
    CHECK("switch&kind=domino" == r["worker_options"]);

    // a retry with the same key gets them as well
    Call(herd, "POST", "/register", "request_key=wo-1");
    CHECK("switch&kind=domino" == Parse(Call(herd, "POST", "/register", "request_key=wo-1"))["worker_options"]);
}

// Job parameters travel with a run command, unchanged, and only with it
static void TestParams()
{
    Herd herd;
    Load(herd, 2);
    Call(herd, "POST", "/register");

    Form c = Parse(Call(herd, "POST", "/command", "test_id=1&command=run&job=mailtest&params=mailsize%3D20-50%26mailattach%3D2"));
    CHECK("1" == c["command_id"]);

    Form s = Parse(Call(herd, "POST", "/status", "test_id=1&state=idle&ack_command_id=0"));
    CHECK("run" == s["command"] && "mailtest" == s["job"] && "mailsize=20-50&mailattach=2" == s["params"]);
    CHECK("mailsize=20-50&mailattach=2" == Parse(Call(herd, "GET", "/client?test_id=1"))["params"]);

    // only with run, no blanks, not too long, not empty
    CHECK(400 == Call(herd, "POST", "/command", "test_id=1&command=pause&pause_seconds=5&params=a%3D1").status);
    CHECK(400 == Call(herd, "POST", "/command", "test_id=1&command=run&job=mail&params=a%20b").status);
    CHECK(400 == Call(herd, "POST", "/command", "test_id=1&command=run&job=mail&params=" + std::string(1025, 'x')).status);
    CHECK(400 == Call(herd, "POST", "/command", "test_id=1&command=run&job=mail&params=").status);
    CHECK(200 == Call(herd, "POST", "/command", "test_id=1&command=run&job=mail&params=" + std::string(1024, 'x')).status);

    // the next command without params has none
    Call(herd, "POST", "/command", "test_id=1&command=run&job=dbopen");
    s = Parse(Call(herd, "POST", "/status", "test_id=1&state=idle&ack_command_id=0"));
    CHECK("dbopen" == s["job"] && s["params"].empty() && s.count("params"));
}

// The worker repeats its last job result with every report: it is counted once, per job
static void TestJobResults()
{
    Herd herd;
    Load(herd, 2);

    Call(herd, "POST", "/register");
    Call(herd, "POST", "/command", "test_id=1&command=run&job=mail");   // command 1
    Call(herd, "POST", "/status", "test_id=1&state=running&ack_command_id=1&message=mail%20%233");

    // validation: all three fields or none, a real result, an id that was issued
    CHECK(400 == Call(herd, "POST", "/status", "test_id=1&state=idle&last_job_id=1").status);
    CHECK(400 == Call(herd, "POST", "/status", "test_id=1&state=idle&last_job_id=1&last_job=mail&last_job_result=maybe").status);
    CHECK(400 == Call(herd, "POST", "/status", "test_id=1&state=idle&last_job_id=0&last_job=mail&last_job_result=ok").status);
    CHECK(400 == Call(herd, "POST", "/status", "test_id=1&state=idle&last_job_id=1&last_job=a%20b&last_job_result=ok").status);
    HttpResponse beyond = Call(herd, "POST", "/status", "test_id=1&state=idle&last_job_id=2&last_job=mail&last_job_result=ok");
    CHECK(400 == beyond.status && "invalid_last_job_id" == Parse(beyond)["error"]);

    Form cl = Parse(Call(herd, "GET", "/client?test_id=1"));
    CHECK("0" == cl["last_job_id"] && cl["last_job"].empty() && cl["last_job_result"].empty());

    // the job fails: the client is idle again (not finished), the result is kept with its message
    for (int i = 0; i < 3; i++)
        CHECK(200 == Call(herd, "POST", "/status", "test_id=1&state=idle&ack_command_id=1&message=mail%20failed&last_job_id=1&last_job=mail&last_job_result=failed").status);

    cl = Parse(Call(herd, "GET", "/client?test_id=1"));
    CHECK("idle" == cl["state"] && "1" == cl["last_job_id"] && "mail" == cl["last_job"] && "failed" == cl["last_job_result"]);
    CHECK("mail failed" == cl["last_job_message"]);

    Form s = Parse(Call(herd, "GET", "/status"));
    CHECK("0" == s["jobs_ok"] && "1" == s["jobs_failed"] && "0" == s["jobs_stopped"] && "0" == s["clients_error"]);

    // the client can still be given work; the next job is stopped by an idle command
    CHECK(200 == Call(herd, "POST", "/command", "test_id=1&command=run&job=agent").status);   // command 2
    Call(herd, "POST", "/status", "test_id=1&state=running&ack_command_id=2&message=agent%20run&last_job_id=1&last_job=mail&last_job_result=failed");
    CHECK(200 == Call(herd, "POST", "/command", "test_id=1&command=idle").status);            // command 3
    Call(herd, "POST", "/status", "test_id=1&state=idle&ack_command_id=2&message=job%20agent%20stopped&last_job_id=2&last_job=agent&last_job_result=stopped");
    Call(herd, "POST", "/status", "test_id=1&state=idle&ack_command_id=3&message=waiting&last_job_id=2&last_job=agent&last_job_result=stopped");

    cl = Parse(Call(herd, "GET", "/client?test_id=1"));
    CHECK("2" == cl["last_job_id"] && "agent" == cl["last_job"] && "stopped" == cl["last_job_result"] && "job agent stopped" == cl["last_job_message"]);

    // a late report with an older job does not go back or count again
    Call(herd, "POST", "/status", "test_id=1&state=idle&ack_command_id=3&last_job_id=1&last_job=mail&last_job_result=ok");
    cl = Parse(Call(herd, "GET", "/client?test_id=1"));
    CHECK("2" == cl["last_job_id"] && "stopped" == cl["last_job_result"]);

    s = Parse(Call(herd, "GET", "/status"));
    CHECK("0" == s["jobs_ok"] && "1" == s["jobs_failed"] && "1" == s["jobs_stopped"]);

    const std::string m = Call(herd, "GET", "/metrics").body;
    CHECK(m.find("# TYPE nshtestherd_jobs_ended_total counter") != std::string::npos);
    CHECK(m.find("nshtestherd_jobs_ended_total{result=\"ok\"} 0\n") != std::string::npos);
    CHECK(m.find("nshtestherd_jobs_ended_total{result=\"failed\"} 1\n") != std::string::npos);
    CHECK(m.find("nshtestherd_jobs_ended_total{result=\"stopped\"} 1\n") != std::string::npos);
    CHECK(m.find("agent") == std::string::npos);   // job names are not labels
}

static void TestMetrics()
{
    Herd herd;
    Load(herd, 4);
    Call(herd, "POST", "/load", "bad\n"); // failure counted
    Call(herd, "POST", "/register", "request_key=secretkey");
    Call(herd, "POST", "/register");
    Call(herd, "POST", "/register");
    Call(herd, "POST", "/register");
    Call(herd, "POST", "/register"); // exhausted
    Call(herd, "POST", "/status", "test_id=1&state=running&message=topsecretmsg");
    Call(herd, "POST", "/status", "test_id=2&state=idle");
    Call(herd, "POST", "/status", "test_id=3&state=error");
    Call(herd, "POST", "/command", "target=all&command=run&job=secretjob"); // clients 1,2,4 (3 is terminal)

    HttpResponse m = Call(herd, "GET", "/metrics", "", "application/json");
    CHECK(m.contentType.find("text/plain; version=0.0.4") == 0);

    const std::string &b = m.body;
    CHECK(b.find("# TYPE nshtestherd_clients_by_state gauge") != std::string::npos);
    CHECK(b.find("nshtestherd_users_total 4\n") != std::string::npos);
    CHECK(b.find("nshtestherd_users_available 0\n") != std::string::npos);
    CHECK(b.find("nshtestherd_users_allocated 4\n") != std::string::npos);
    CHECK(b.find("nshtestherd_clients_total 4\n") != std::string::npos);
    CHECK(b.find("clients_by_state{state=\"registered\"} 1\n") != std::string::npos);
    CHECK(b.find("clients_by_state{state=\"idle\"} 1\n") != std::string::npos);
    CHECK(b.find("clients_by_state{state=\"running\"} 1\n") != std::string::npos);
    CHECK(b.find("clients_by_state{state=\"paused\"} 0\n") != std::string::npos);
    CHECK(b.find("clients_by_state{state=\"stopping\"} 0\n") != std::string::npos);
    CHECK(b.find("clients_by_state{state=\"done\"} 0\n") != std::string::npos);
    CHECK(b.find("clients_by_state{state=\"error\"} 1\n") != std::string::npos);
    CHECK(b.find("nshtestherd_registrations_total 4\n") != std::string::npos);
    CHECK(b.find("nshtestherd_allocation_failures_total 1\n") != std::string::npos);
    CHECK(b.find("nshtestherd_status_reports_total 3\n") != std::string::npos);
    CHECK(b.find("nshtestherd_command_updates_total 3\n") != std::string::npos);
    CHECK(b.find("nshtestherd_csv_loads_total 1\n") != std::string::npos);
    CHECK(b.find("nshtestherd_csv_load_failures_total 1\n") != std::string::npos);

    // no credentials or high-cardinality values anywhere in metrics or summary
    HttpResponse s = Call(herd, "GET", "/status");

    for (const std::string *text : { &b, (const std::string *)&s.body })
    {
        CHECK(text->find("Secret") == std::string::npos);
        CHECK(text->find("load0000") == std::string::npos);
        CHECK(text->find("example.com") == std::string::npos);
        CHECK(text->find("secretkey") == std::string::npos);
        CHECK(text->find("topsecretmsg") == std::string::npos);
        CHECK(text->find("secretjob") == std::string::npos);
    }

    // per-state counts in /status agree with metrics
    Form sf = Parse(s);
    CHECK("1" == sf["clients_registered"] && "1" == sf["clients_idle"] && "1" == sf["clients_running"] && "1" == sf["clients_error"]);
    CHECK("4" == sf["clients_total"]);
}

static void TestGenerate()
{
    std::vector<UserRecord> u = GenerateUsers(3, "load", "TestPassword", "example.com");
    CHECK(3 == u.size());
    CHECK("Load" == u[0].firstName && "000001" == u[0].lastName && "TestPassword" == u[0].password);
    CHECK("load000001" == u[0].shortName && "load000001@example.com" == u[0].internetAddress);
    CHECK("load000003" == u[2].shortName);

    // width grows past 6 digits, shortnames stay unique
    CHECK("1000000" == GenerateUsers(1000000, "x", "p", "d").back().lastName);

    // a generated pool behaves like a loaded CSV: bounded, exhausts, locked after first registration
    Herd   herd;
    size_t count = 0;
    CHECK(herd.LoadUsers(GenerateUsers(2, "t", "pw", "ex.org"), count).Ok() && 2 == count);
    CHECK("t000001" == Parse(Call(herd, "POST", "/register"))["shortname"]);
    CHECK("t000002@ex.org" == Parse(Call(herd, "POST", "/register"))["internetaddress"]);
    CHECK(409 == Call(herd, "POST", "/register").status);
    CHECK(409 == herd.LoadUsers(GenerateUsers(5, "t", "pw", "ex.org"), count).code);
}

// Standard section header: blank line, rule, title, rule, blank line
static void Header(const char *title)
{
    const std::string rule(90, '-');
    std::printf("\n%s\n%s\n%s\n\n", rule.c_str(), title, rule.c_str());
}

// One result line per test: [ OK ] / [ FAIL ]
static void RunTest(const char *name, void (*test)())
{
    int failuresBefore = g_failures;
    int checksBefore   = g_checks;

    test();

    if (g_failures == failuresBefore)
        std::printf("[ OK ]   %s (%d checks)\n", name, g_checks - checksBefore);
    else
        std::printf("[ FAIL ] %s (%d of %d checks failed)\n", name, g_failures - failuresBefore, g_checks - checksBefore);
}

int main()
{
    Header("test_core: logic tests (no sockets)");

    RunTest("CSV parsing and validation", TestCsv);
    RunTest("wire format and form decoding", TestWire);
    RunTest("generated accounts", TestGenerate);
    RunTest("load rollback and replace policy", TestLoadRollbackAndPolicy);
    RunTest("registration and retry keys", TestRegistration);
    RunTest("concurrent allocation", TestConcurrentAllocation);
    RunTest("commands and state", TestCommandsAndState);
    RunTest("job results", TestJobResults);
    RunTest("token", TestToken);
    RunTest("job parameters", TestParams);
    RunTest("worker options", TestWorkerOptions);
    RunTest("metrics", TestMetrics);

    Header("Summary");
    std::printf("%s %d checks, %d failures\n\n", g_failures ? "[ FAIL ]" : "[ OK ]  ", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
