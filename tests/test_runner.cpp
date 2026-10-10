// test_runner.cpp - runner against a real in-process coordinator over local HTTP
//
// Uses 127.0.0.1:HERD_TEST_PORT (default 18790). If the port is busy the test
// is skipped (exit 0) rather than touching whatever is listening there.
// Dummy-job scenario runs everywhere; the external-program scenarios use /bin/sh
// and are POSIX only.

#include "../src/api.h"
#include "../src/herdclient.h"
#include "../src/httpclient.h"
#include "../src/process.h"
#include "../src/runner.h"
#include "../src/wire.h"

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <thread>

static int g_failures = 0;
static int g_checks   = 0;

#define CHECK(cond)                                                    \
    do                                                                 \
    {                                                                  \
        g_checks++;                                                    \
        if (!(cond))                                                   \
        {                                                              \
            g_failures++;                                              \
            std::printf("[ FAIL ] %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                              \
    } while (0)

static HttpUrl g_url;

static Form Call(const std::string &method, const std::string &path, const std::string &body = "")
{
    HttpReply   reply;
    std::string err;
    Form        f;

    if (HttpCall(g_url, method, path, body, reply, err, 5))
        ParseTextFields(reply.body, f);

    return f;
}

static bool WaitFor(std::function<bool()> cond, int timeoutMs)
{
    for (int waited = 0; waited < timeoutMs; waited += 50)
    {
        if (cond())
            return true;

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    return cond();
}

static Form Client(int id)
{
    return Call("GET", "/client?test_id=" + std::to_string(id));
}

static bool AllClients(int count, const std::string &state, const std::string &ack)
{
    for (int i = 1; i <= count; i++)
    {
        Form c = Client(i);

        if (c["state"] != state || c["ack_command_id"] != ack)
            return false;
    }

    return true;
}

struct TestServer
{
    Herd         herd;
    HttpServer   server;
    std::thread  thread;
    std::string  token;   // set before Start(): every call but /health then needs it
    std::atomic<int> rejectReports;   // the next n worker status reports (POST /status) get a 503, as if the network failed

    explicit TestServer(int port)
        : server(MakeConfig(port), [this](const HttpRequest &r) { return Handle(r); }), rejectReports(0)
    {
    }

    HttpResponse Handle(const HttpRequest &r)
    {
        if ("POST" == r.method && "/status" == r.path && rejectReports > 0)
        {
            rejectReports--;

            HttpResponse down;
            down.status = 503;
            return down;
        }

        return HandleRequest(herd, r, token);
    }

    static ServerConfig MakeConfig(int port)
    {
        ServerConfig c;
        c.port           = port;
        c.timeoutSeconds = 3;
        return c;
    }

    bool Start(int users)
    {
        std::string err;

        if (!server.Start(err))
        {
            std::printf("SKIP: %s\n", err.c_str());
            return false;
        }

        std::string csv;

        for (int i = 1; i <= users; i++)
            csv += "Load," + std::to_string(i) + ",TestPw,load" + std::to_string(i) + ",load" + std::to_string(i) + "@example.com\n";

        size_t count = 0;
        herd.LoadCsv(csv, count);
        thread = std::thread([this]() { server.Run(); });
        return true;
    }

    ~TestServer()
    {
        server.Stop();

        if (thread.joinable())
            thread.join();
    }
};

static RunnerConfig MakeRunner(int clients)
{
    RunnerConfig c;
    c.serverUrl = "http://127.0.0.1:" + std::to_string(g_url.port);
    c.clients   = clients;
    c.pollMs    = 100;
    return c;
}

static void TestDummyClients()
{
    TestServer srv(g_url.port);

    if (!srv.Start(5))
        exit(0);

    RunnerConfig      cfg = MakeRunner(3);
    std::atomic<bool> stop(false);
    int               rc = -1;
    std::thread       rt([&]() { rc = RunRunner(cfg, stop); });

    // Each logical client registers separately: ids 1..3, distinct accounts
    CHECK(WaitFor([]() { return Call("GET", "/status")["clients_total"] == "3"; }, 10000));
    CHECK("2" == Call("GET", "/status")["users_available"]);
    CHECK(Client(1)["shortname"] != Client(2)["shortname"]);

    Call("POST", "/command", "target=all&command=run&job=dummy-job");
    CHECK(WaitFor([]() { return AllClients(3, "running", "1"); }, 10000));

    // Timed pause: reported state follows, desired instruction stays "pause" after it expires
    Call("POST", "/command", "target=all&command=pause&pause_seconds=1");
    CHECK(WaitFor([]() { return AllClients(3, "paused", "2"); }, 10000));
    CHECK(WaitFor([]() { return AllClients(3, "running", "2"); }, 10000));
    CHECK("pause" == Client(1)["command"] && "2" == Client(1)["command_id"]);

    // Metrics agree with the clients
    CHECK(WaitFor([]() { return Call("GET", "/status")["clients_running"] == "3"; }, 5000));

    Call("POST", "/command", "target=all&command=stop");
    CHECK(WaitFor([]() { return Call("GET", "/status")["clients_done"] == "3"; }, 10000));
    rt.join();
    CHECK(0 == rc);
}

// Without a client count the runner keeps registering until the pool is booked
static void TestFillPool()
{
    TestServer srv(g_url.port);

    if (!srv.Start(4))
        exit(0);

    RunnerConfig      cfg = MakeRunner(0);
    std::atomic<bool> stop(false);
    int               rc = -1;
    std::thread       rt([&]() { rc = RunRunner(cfg, stop); });

    CHECK(WaitFor([]() { Form s = Call("GET", "/status"); return s["clients_total"] == "4" && s["users_available"] == "0"; }, 10000));

    // Exactly the pool size, never more: no further registration is possible
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK("4" == Call("GET", "/status")["clients_total"]);
    Form c5 = Client(5);
    CHECK("1" == Client(1)["test_id"] && "4" == Client(4)["test_id"] && c5.find("test_id") == c5.end());

    Call("POST", "/command", "target=all&command=stop");
    CHECK(WaitFor([]() { return Call("GET", "/status")["clients_done"] == "4"; }, 10000));
    rt.join();
    CHECK(0 == rc);
}

// Default mode is capped by maxClients so several machines can share one pool
static void TestFillCap()
{
    TestServer srv(g_url.port);

    if (!srv.Start(4))
        exit(0);

    RunnerConfig cfg = MakeRunner(0);
    cfg.maxClients   = 2;

    std::atomic<bool> stop(false);
    int               rc = -1;
    std::thread       rt([&]() { rc = RunRunner(cfg, stop); });

    CHECK(WaitFor([]() { return Call("GET", "/status")["clients_total"] == "2"; }, 10000));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK("2" == Call("GET", "/status")["clients_total"] && "2" == Call("GET", "/status")["users_available"]);

    Call("POST", "/command", "target=all&command=stop");
    rt.join();
    CHECK(0 == rc);
}

// The runner starts nshtestherd itself as the child (--child-info prints its pid) and
// reports the exit. Skipped if the binary has not been built. Works on Windows too.
static void TestSelfChild()
{
#ifdef _WIN32
    const char *def = ".\nshtestherd.exe";
#else
    const char *def = "./nshtestherd";
#endif
    const char *bin = std::getenv("HERD_TEST_BIN");
    std::string path = bin ? bin : def;

    if (!std::ifstream(path.c_str()))
    {
        std::printf("SKIP self-child test: %s not found (run make first)\n", path.c_str());
        return;
    }

    TestServer srv(g_url.port);

    if (!srv.Start(2))
        exit(0);

    RunnerConfig cfg = MakeRunner(1);
    cfg.program      = path;
    cfg.programArgs  = { "--child-info" };

    std::atomic<bool> stop(false);
    int               rc = -1;
    std::thread       rt([&]() { rc = RunRunner(cfg, stop); });

    CHECK(WaitFor([]() { return Call("GET", "/status")["clients_total"] == "1"; }, 10000));
    Call("POST", "/command", "test_id=1&command=run&job=selfjob");
    CHECK(WaitFor([]() { Form c = Client(1); return c["state"] == "idle" && c["ack_command_id"] == "1" && c["message"].find("finished (exit 0)") != std::string::npos; }, 10000));

    Call("POST", "/command", "test_id=1&command=stop");
    CHECK(WaitFor([]() { return Client(1)["state"] == "done"; }, 10000));
    rt.join();
    CHECK(0 == rc);
}

// Serves one canned HTTP reply on 127.0.0.1:port (read the request, send raw, close), then calls it.
static bool CallRaw(const std::string &rawReply, HttpReply &reply, std::string &err)
{
    SocketHandle listener = socket(AF_INET, SOCK_STREAM, 0);

#ifndef _WIN32
    int one = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#endif

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((unsigned short)g_url.port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (0 != bind(listener, (struct sockaddr *)&addr, sizeof(addr)) || 0 != listen(listener, 1))
    {
        SockClose(listener);
        err = "cannot bind test port";
        return false;
    }

    std::thread server([&]()
    {
        SocketHandle c = accept(listener, NULL, NULL);

        if (INVALID_SOCK == c)
            return;

        SockSetTimeout(c, 3);

        std::string request;
        char        buf[1024];

        while (request.find("\r\n\r\n") == std::string::npos)
        {
            long n = (long)recv(c, buf, sizeof(buf), 0);

            if (n <= 0)
                break;

            request.append(buf, (size_t)n);
        }

        send(c, rawReply.data(), (int)rawReply.size(), NSH_SEND_FLAGS);
        SockClose(c);
    });

    bool ok = HttpCall(g_url, "GET", "/x", "", reply, err, 3);
    server.join();
    SockClose(listener);
    return ok;
}

// A reply cut off by a dropped connection must not look successful
static void TestReplyFraming()
{
    HttpReply   reply;
    std::string err;

    CHECK(!CallRaw("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\ntest_id=1\n", reply, err));
    CHECK(err.find("truncated") != std::string::npos);

    CHECK(CallRaw("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\ntest_id=1\n", reply, err));
    CHECK(200 == reply.status && "test_id=1\n" == reply.body);

    // extra bytes beyond Content-Length are dropped
    CHECK(CallRaw("HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabcdef", reply, err));
    CHECK("abc" == reply.body);

    CHECK(!CallRaw("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n", reply, err));
    CHECK(!CallRaw("HTTP/1.1 2x0 OK\r\nContent-Length: 0\r\n\r\n", reply, err));
    CHECK(!CallRaw("garbage", reply, err));
}

#ifndef _WIN32
// A child that ignores SIGTERM must still be gone (SIGKILL, reaped) after StopAndReap
static void TestStubbornChild()
{
    ChildProcess child;
    std::string  err;

    CHECK(child.Start("/bin/sh", { "-c", "trap '' TERM; while :; do sleep 1; done" }, EnvList(), err));

    long long pid = child.Pid();
    CHECK(pid > 0);

    // give the shell time to install the trap
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    child.StopAndReap(300);

    CHECK(!child.Running());
    CHECK(-1 == kill((pid_t)pid, 0) && ESRCH == errno); // reaped: no zombie, no orphan
}


static void TestExternalProgram()
{
    const std::string countFile = "/tmp/nshherd_runner_test_" + std::to_string((long)getpid());
    std::remove(countFile.c_str());
    setenv("HERD_TEST_COUNT", countFile.c_str(), 1);

    TestServer srv(g_url.port);

    if (!srv.Start(2))
        exit(0);

    RunnerConfig cfg = MakeRunner(1);
    cfg.program      = "/bin/sh";
    // The NSHTEST_* names, the job parameters, and the old NSH_* names that are still set for older programs
    cfg.programArgs  = { "-c", "echo run >> \"$HERD_TEST_COUNT\"; test \"$NSHTEST_SHORTNAME\" = load1 && test \"$NSHTEST_PASSWORD\" = TestPw && "
                               "test \"$NSHTEST_JOB\" = envjob && test \"$NSHTEST_PARAMS\" = 'a=1&b=2' && test \"$NSHTEST_COMMAND_ID\" = 1 && "
                               "test \"$NSHTEST_TEST_ID\" = 1 && test \"$NSH_SHORTNAME\" = load1 && test \"$NSH_PASSWORD\" = TestPw" };

    std::atomic<bool> stop(false);
    int               rc = -1;
    std::thread       rt([&]() { rc = RunRunner(cfg, stop); });

    CHECK(WaitFor([]() { return Call("GET", "/status")["clients_total"] == "1"; }, 10000));

    // Successful child: account, job and parameters arrive via environment, client returns to idle
    Call("POST", "/command", "test_id=1&command=run&job=envjob&params=a%3D1%26b%3D2");
    CHECK(WaitFor([]() { Form c = Client(1); return c["state"] == "idle" && c["ack_command_id"] == "1" && c["message"].find("finished (exit 0)") != std::string::npos; }, 10000));

    // Already acknowledged command id: the finished child must not be restarted
    std::this_thread::sleep_for(std::chrono::milliseconds(800));

    int lines = 0;
    {
        std::ifstream in(countFile);
        std::string   line;

        while (std::getline(in, line))
            lines++;
    }

    CHECK(1 == lines);
    CHECK("idle" == Client(1)["state"]);

    // A new run command is a new execution; this one fails (job and parameters mismatch): reported as failed, the client
    // stays idle and waits for work
    Call("POST", "/command", "test_id=1&command=run&job=otherjob");
    CHECK(WaitFor([]() { Form c = Client(1); return c["state"] == "idle" && c["last_job_id"] == "2"; }, 10000));
    Form failed = Client(1);
    CHECK("otherjob" == failed["last_job"] && "failed" == failed["last_job_result"]);
    CHECK(failed["last_job_message"].find("failed (exit 1)") != std::string::npos);
    CHECK("1" == Call("GET", "/status")["jobs_ok"] && "1" == Call("GET", "/status")["jobs_failed"]);

    Call("POST", "/command", "test_id=1&command=stop");
    CHECK(WaitFor([]() { return Client(1)["state"] == "done"; }, 10000));
    rt.join();
    CHECK(0 == rc);

    std::remove(countFile.c_str());
}

static void TestMissingProgram()
{
    TestServer srv(g_url.port);

    if (!srv.Start(1))
        exit(0);

    RunnerConfig cfg = MakeRunner(1);
    cfg.program      = "/nonexistent/nshherd-test-program";

    std::atomic<bool> stop(false);
    int               rc = -1;
    std::thread       rt([&]() { rc = RunRunner(cfg, stop); });

    CHECK(WaitFor([]() { return Call("GET", "/status")["clients_total"] == "1"; }, 10000));
    Call("POST", "/command", "test_id=1&command=run&job=x");
    CHECK(WaitFor([]() { Form c = Client(1); return c["state"] == "idle" && c["last_job_result"] == "failed"; }, 10000));

    Call("POST", "/command", "test_id=1&command=stop");
    CHECK(WaitFor([]() { return Client(1)["state"] == "done"; }, 10000));
    rt.join();
    CHECK(0 == rc);
}

// The placeholders really arrive as arguments of the child (and the environment is still passed as well)
static void TestTemplatedChild()
{
    TestServer srv(g_url.port);

    if (!srv.Start(2))
        exit(0);

    RunnerConfig cfg = MakeRunner(1);
    cfg.program      = "/bin/sh";
    // The last placeholder uses the old name: it must still work
    cfg.programArgs  = { "-c", "test \"$#\" = 3 && test \"$1\" = load1 && test \"$2\" = tpljob && test \"$3\" = 1 && test \"$NSHTEST_SHORTNAME\" = load1",
                         "sh", "{NSHTEST_SHORTNAME}", "{NSHTEST_JOB}", "{NSH_TEST_ID}" };

    std::atomic<bool> stop(false);
    int               rc = -1;
    std::thread       rt([&]() { rc = RunRunner(cfg, stop); });

    CHECK(WaitFor([]() { return Call("GET", "/status")["clients_total"] == "1"; }, 10000));
    Call("POST", "/command", "test_id=1&command=run&job=tpljob");
    CHECK(WaitFor([]() { Form c = Client(1); return c["state"] == "idle" && c["message"].find("finished (exit 0)") != std::string::npos; }, 10000));

    Call("POST", "/command", "test_id=1&command=stop");
    CHECK(WaitFor([]() { return Client(1)["state"] == "done"; }, 10000));
    rt.join();
    CHECK(0 == rc);
}

#endif

// {NSHTEST_...} placeholders in the program arguments (pure string handling, no processes)
static void TestArgTemplates()
{
    EnvList values = {
        { "NSHTEST_TEST_ID", "7" },
        { "NSHTEST_SHORTNAME", "load000007" },
        { "NSHTEST_JOB", "mail read" },
        { "NSH_SHORTNAME", "load000007" },     // the old name, still set by the runner
    };

    std::string out;
    std::string err;

    CHECK(ExpandArgTemplate("--user={NSHTEST_SHORTNAME}", values, out, err) && "--user=load000007" == out);
    CHECK(ExpandArgTemplate("{NSHTEST_TEST_ID}-{NSHTEST_SHORTNAME}-{NSHTEST_TEST_ID}", values, out, err) && "7-load000007-7" == out);
    CHECK(ExpandArgTemplate("no placeholder", values, out, err) && "no placeholder" == out);
    CHECK(ExpandArgTemplate("--user={NSH_SHORTNAME}", values, out, err) && "--user=load000007" == out);   // old name

    // a value with a space stays inside its single argument
    CHECK(ExpandArgTemplate("{NSHTEST_JOB}", values, out, err) && "mail read" == out);

    // anything that is not a complete {NSHTEST_NAME} (or old {NSH_NAME}) stays literal (JSON, shell-like text, lone braces)
    CHECK(ExpandArgTemplate("{\"a\":1}", values, out, err) && "{\"a\":1}" == out);
    CHECK(ExpandArgTemplate("{NSHTEST_ open", values, out, err) && "{NSHTEST_ open" == out);
    CHECK(ExpandArgTemplate("{NSHTEST_lower}", values, out, err) && "{NSHTEST_lower}" == out);
    CHECK(ExpandArgTemplate("{NSHX}", values, out, err) && "{NSHX}" == out);
    CHECK(ExpandArgTemplate("{NSHOTHER_NAME}", values, out, err) && "{NSHOTHER_NAME}" == out);           // another prefix
    CHECK(ExpandArgTemplate("$NSHTEST_SHORTNAME", values, out, err) && "$NSHTEST_SHORTNAME" == out);     // shell syntax is not ours

    // values are inserted as they are, not expanded a second time
    EnvList tricky = { { "NSHTEST_JOB", "{NSHTEST_TEST_ID}" }, { "NSHTEST_TEST_ID", "7" } };
    CHECK(ExpandArgTemplate("{NSHTEST_JOB}", tricky, out, err) && "{NSHTEST_TEST_ID}" == out);

    // errors
    CHECK(!ExpandArgTemplate("{NSHTEST_UNKNOWN}", values, out, err) && err.find("unknown placeholder") != std::string::npos);
    CHECK(!ExpandArgTemplate("--pw={NSHTEST_PASSWORD}", values, out, err) && err.find("not allowed") != std::string::npos);
    CHECK(!ExpandArgTemplate("--pw={NSH_PASSWORD}", values, out, err) && err.find("not allowed") != std::string::npos);

    // startup validation knows every documented name (new and old), and refuses the rest
    std::vector<std::string> good = { "{NSHTEST_TEST_ID}", "{NSHTEST_FIRSTNAME}", "{NSHTEST_LASTNAME}", "{NSHTEST_SHORTNAME}",
                                      "{NSHTEST_INTERNETADDRESS}", "{NSHTEST_JOB}", "{NSHTEST_PARAMS}", "{NSHTEST_COMMAND_ID}",
                                      "{NSHTEST_SERVER}", "{NSH_SHORTNAME}", "{NSH_TEST_ID}", "{NSH_JOB}", "plain" };
    CHECK(ValidateArgTemplates(good, err));
    CHECK(!ValidateArgTemplates({ "--x", "{NSHTEST_TYPO}" }, err));
    CHECK(!ValidateArgTemplates({ "{NSH_PARAMS}" }, err));        // new, so only under the new name
    CHECK(!ValidateArgTemplates({ "{NSHTEST_PASSWORD}" }, err));
    CHECK(!ValidateArgTemplates({ "{NSH_PASSWORD}" }, err));
}

// A program that is not the generic runner (domlem is one) uses HerdClient with its own hooks.
// This one counts job steps, can fail a job and can fail its identity setup.
class CustomHooks : public HerdHooks
{
public:
    std::atomic<bool> stop;
    std::atomic<int>  steps;
    std::atomic<int>  failAfter;   // fail the job at this step; 0: never
    std::atomic<bool> setupFails;
    std::atomic<bool> longMessage;  // job steps report a message over the coordinator's limit
    std::atomic<int>  waiting;      // calls of Waiting()
    std::atomic<int>  aborts;       // calls of JobAbort()
    std::atomic<int>  stateChanges; // calls of StateChanged()
    std::atomic<bool> startFails;   // JobStart() refuses the job
    std::atomic<int> *rejectOnAbort; // JobAbort() makes the coordinator refuse this many reports (the network fails right then)
    std::string       waitReason;  // written by the client thread: read it after join()
    std::string       registeredAs; // written by the client thread: read it after join()
    std::string       startedJob;
    std::string       startedParams; // written by the client thread: read it after join()
    std::string       workerOptions; // written by the client thread: read it after join()
    std::string       lastState;    // written by the client thread: read it after join()

    CustomHooks() : stop(false), steps(0), failAfter(0), setupFails(false), longMessage(false), waiting(0), aborts(0), stateChanges(0), startFails(false), rejectOnAbort(NULL) {}

    void StateChanged(const std::string &state, const std::string &) override
    {
        lastState = state;
        stateChanges++;
    }

    void JobAbort() override
    {
        aborts++;

        if (rejectOnAbort)
            *rejectOnAbort = 1;
    }

    void Waiting(const std::string &reason) override
    {
        waitReason = reason;
        waiting++;
    }

    bool Stopping() override { return stop; }
    void Sleep(int ms) override { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
    void Log(const std::string &, const std::string &) override {}

    bool Registered(const HerdAccount &account, std::string &err) override
    {
        registeredAs  = account.shortName;
        workerOptions = account.workerOptions;

        if (setupFails)
        {
            err = "no vault";
            return false;
        }

        return true;
    }

    bool JobStart(const HerdJobContext &context, std::string &message, std::string &err) override
    {
        startedJob    = context.job;
        startedParams = context.params;
        message       = "custom " + context.job;

        if (startFails)
        {
            err = "cannot start";
            return false;
        }

        return true;
    }

    HerdJobResult JobStep(std::string &message) override
    {
        int n   = ++steps;
        message = "step " + std::to_string(n);

        // One byte less than the limit, then a two byte UTF-8 character across it
        if (longMessage)
            message = std::string(MAX_STATUS_MESSAGE_BYTES - 1, 'x') + "\xC3\xA4";

        if (failAfter > 0 && n >= failAfter)
            return HERD_JOB_FAILED;

        return HERD_JOB_RUNNING;
    }
};

static HerdClientConfig CustomConfig(const char *key)
{
    HerdClientConfig c;
    c.serverUrl  = "http://127.0.0.1:" + std::to_string(g_url.port);
    c.requestKey = key;
    c.name       = "testprog";
    c.pollMs     = 50;
    return c;
}

// register (with worker options), run, pause (the steps must stand still), resume, stop
static void TestHerdClientHooks()
{
    TestServer srv(g_url.port);
    srv.herd.SetWorkerOptions("switch&other=1");

    if (!srv.Start(3))
        exit(0);

    CustomHooks      hooks;
    HerdClientConfig cfg = CustomConfig("hooks-test-1");
    HerdEnd          end = HERD_END_FAILED;
    std::thread      rt([&]() { HerdClient client(cfg, hooks); end = client.Run(); });

    CHECK(WaitFor([]() { return Call("GET", "/status")["clients_total"] == "1"; }, 10000));

    Call("POST", "/command", "test_id=1&command=run&job=custom&params=mailsize%3D20-50%26mailattach%3D2");
    CHECK(WaitFor([&]() { return Client(1)["state"] == "running" && hooks.steps >= 3; }, 10000));
    CHECK("step" == Client(1)["message"].substr(0, 4));

    // while paused no job step may run, however long the pause lasts
    Call("POST", "/command", "test_id=1&command=pause&pause_seconds=1");
    CHECK(WaitFor([]() { return Client(1)["state"] == "paused"; }, 10000));

    int pausedSteps = hooks.steps;
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    CHECK(pausedSteps == hooks.steps);

    CHECK(WaitFor([]() { return Client(1)["state"] == "running"; }, 10000));
    CHECK(WaitFor([&]() { return hooks.steps > pausedSteps; }, 10000));

    Call("POST", "/command", "test_id=1&command=stop");
    CHECK(WaitFor([]() { return Client(1)["state"] == "done"; }, 10000));
    rt.join();

    CHECK(HERD_END_CLEAN == end);
    CHECK("load1" == hooks.registeredAs);
    CHECK("custom" == hooks.startedJob);
    CHECK("mailsize=20-50&mailattach=2" == hooks.startedParams);   // the job parameters arrive unchanged
    CHECK("switch&other=1" == hooks.workerOptions);                // so do the worker options, before the first command
}


// A coordinator with a token: a client with the token works, one without it is refused and ends
static void TestClientToken()
{
    TestServer srv(g_url.port);
    srv.token = "herd-test-token-0123456789";

    if (!srv.Start(2))
        exit(0);

    CustomHooks      good;
    CustomHooks      bad;
    HerdClientConfig cfgGood = CustomConfig("token-good");
    HerdClientConfig cfgBad  = CustomConfig("token-bad");
    HerdEnd          endGood = HERD_END_FAILED;
    HerdEnd          endBad  = HERD_END_CLEAN;

    cfgGood.token = srv.token;

    std::thread tBad([&]() { HerdClient client(cfgBad, bad); endBad = client.Run(); });
    tBad.join();
    CHECK(HERD_END_FAILED == endBad);
    CHECK(bad.registeredAs.empty());

    std::thread tGood([&]() { HerdClient client(cfgGood, good); endGood = client.Run(); });

    auto status = [&]()
    {
        HttpReply   reply;
        std::string err;
        Form        f;

        if (HttpCall(g_url, "GET", "/status", "", reply, err, 5, srv.token))
            ParseTextFields(reply.body, f);

        return f;
    };

    CHECK(WaitFor([&]() { return status()["clients_idle"] == "1"; }, 10000));

    Form anonymous = Call("GET", "/status");   // without the token: only the error
    CHECK(anonymous.find("clients_total") == anonymous.end() && "unauthorized" == anonymous["error"]);

    HttpReply   reply;
    std::string err;
    HttpCall(g_url, "POST", "/command", "test_id=1&command=stop", reply, err, 5, srv.token);
    tGood.join();
    CHECK(HERD_END_CLEAN == endGood);
    CHECK("load1" == good.registeredAs);
}

// A failing job is reported and the client waits for the next command; a newer command stops a running job.
// Only a failing identity setup ends the client in error.
static void TestHerdClientFailures()
{
    {
        TestServer srv(g_url.port);

        if (!srv.Start(2))
            exit(0);

        CustomHooks      hooks;
        HerdClientConfig cfg = CustomConfig("hooks-test-2");
        HerdEnd          end = HERD_END_FAILED;
        hooks.failAfter      = 2;
        std::thread rt([&]() { HerdClient client(cfg, hooks); end = client.Run(); });

        CHECK(WaitFor([]() { return Call("GET", "/status")["clients_total"] == "1"; }, 10000));
        CHECK(WaitFor([]() { return Client(1)["message"] == "waiting for work"; }, 10000));

        // command 1 fails at step 2: reported once as failed, the client is idle and stays
        Call("POST", "/command", "test_id=1&command=run&job=custom");
        CHECK(WaitFor([]() { Form c = Client(1); return c["state"] == "idle" && c["last_job_result"] == "failed"; }, 10000));
        CHECK("1" == Client(1)["last_job_id"] && "step 2" == Client(1)["last_job_message"]);
        CHECK(0 == hooks.aborts);   // a job that ended by itself is not aborted

        // command 2 runs again, command 3 (idle) stops it: JobAbort, reported as stopped
        hooks.failAfter = 0;
        Call("POST", "/command", "test_id=1&command=run&job=custom");
        CHECK(WaitFor([]() { return Client(1)["state"] == "running"; }, 10000));
        Call("POST", "/command", "test_id=1&command=idle");
        CHECK(WaitFor([]() { Form c = Client(1); return c["state"] == "idle" && c["ack_command_id"] == "3"; }, 10000));

        Form c = Client(1);
        CHECK("2" == c["last_job_id"] && "stopped" == c["last_job_result"] && "job custom stopped" == c["last_job_message"]);
        CHECK(1 == hooks.aborts);

        Form s = Call("GET", "/status");
        CHECK("1" == s["jobs_failed"] && "1" == s["jobs_stopped"] && "0" == s["clients_error"]);

        Call("POST", "/command", "test_id=1&command=stop");
        CHECK(WaitFor([]() { return Client(1)["state"] == "done"; }, 10000));
        rt.join();
        CHECK(HERD_END_CLEAN == end);
        CHECK(hooks.stateChanges > 0 && "done" == hooks.lastState);
    }

    {
        TestServer srv(g_url.port);

        if (!srv.Start(2))
            exit(0);

        CustomHooks      hooks;
        HerdClientConfig cfg = CustomConfig("hooks-test-3");
        HerdEnd          end = HERD_END_CLEAN;
        hooks.setupFails     = true;
        std::thread rt([&]() { HerdClient client(cfg, hooks); end = client.Run(); });

        CHECK(WaitFor([]() { return Client(1)["state"] == "error"; }, 10000));
        CHECK(Client(1)["message"].find("identity setup failed") != std::string::npos);
        rt.join();
        CHECK(HERD_END_FAILED == end);
        CHECK(0 == hooks.steps);
    }
}

// A result the coordinator has not accepted yet must not be replaced by the next one. Here the report of "stopped" (job 1,
// ended by the next run command) fails, and that run command fails to start at once, so job 2 ends before any report went
// through: both results must still be counted.
static void TestResultNotLost()
{
    TestServer srv(g_url.port);

    if (!srv.Start(1))
        exit(0);

    CustomHooks      hooks;
    HerdClientConfig cfg = CustomConfig("result-not-lost");
    HerdEnd          end = HERD_END_FAILED;
    hooks.rejectOnAbort  = &srv.rejectReports;
    std::thread rt([&]() { HerdClient client(cfg, hooks); end = client.Run(); });

    CHECK(WaitFor([]() { return Call("GET", "/status")["clients_total"] == "1"; }, 10000));
    Call("POST", "/command", "test_id=1&command=run&job=custom");
    CHECK(WaitFor([]() { return Client(1)["state"] == "running"; }, 10000));

    hooks.startFails = true;
    Call("POST", "/command", "test_id=1&command=run&job=custom");
    CHECK(WaitFor([]() { Form c = Client(1); return c["state"] == "idle" && c["last_job_id"] == "2" && c["last_job_result"] == "failed"; }, 10000));

    Form s = Call("GET", "/status");
    CHECK("1" == s["jobs_stopped"] && "1" == s["jobs_failed"] && "0" == s["jobs_ok"]);
    CHECK(0 == srv.rejectReports);   // the refused report really happened

    Call("POST", "/command", "test_id=1&command=stop");
    CHECK(WaitFor([]() { return Client(1)["state"] == "done"; }, 10000));
    rt.join();
    CHECK(HERD_END_CLEAN == end);
}

// A message over the coordinator's limit (MAX_STATUS_MESSAGE_BYTES) is cut (not inside a UTF-8 character): the reports are accepted
// and the client still follows commands
static void TestLongMessage()
{
    TestServer srv(g_url.port);

    if (!srv.Start(1))
        exit(0);

    CustomHooks      hooks;
    HerdClientConfig cfg = CustomConfig("long-message");
    HerdEnd          end = HERD_END_FAILED;
    hooks.longMessage    = true;
    std::thread rt([&]() { HerdClient client(cfg, hooks); end = client.Run(); });

    CHECK(WaitFor([]() { return Call("GET", "/status")["clients_total"] == "1"; }, 10000));
    Call("POST", "/command", "test_id=1&command=run&job=custom");
    CHECK(WaitFor([&]() { return Client(1)["state"] == "running" && hooks.steps >= 2; }, 10000));
    CHECK(std::string(MAX_STATUS_MESSAGE_BYTES - 1, 'x') == Client(1)["message"]);

    Call("POST", "/command", "test_id=1&command=stop");
    CHECK(WaitFor([]() { return Client(1)["state"] == "done"; }, 10000));
    rt.join();
    CHECK(HERD_END_CLEAN == end);
}

// A client that finds no free account keeps waiting (the default), and says so; told not to, it ends
static void TestWaitForAccount()
{
    TestServer srv(g_url.port);

    if (!srv.Start(1))
        exit(0);

    CustomHooks      first;
    CustomHooks      second;
    CustomHooks      third;
    HerdClientConfig cfg1 = CustomConfig("wait-1");
    HerdClientConfig cfg2 = CustomConfig("wait-2");
    HerdClientConfig cfg3 = CustomConfig("wait-3");
    HerdEnd          end1 = HERD_END_FAILED;
    HerdEnd          end2 = HERD_END_FAILED;
    HerdEnd          end3 = HERD_END_CLEAN;

    cfg3.waitForAccount = false;   // waiting is the default

    std::thread t1([&]() { HerdClient client(cfg1, first); end1 = client.Run(); });
    CHECK(WaitFor([]() { return Call("GET", "/status")["clients_total"] == "1"; }, 10000));

    // The only account is taken: this one waits and says why
    std::thread t2([&]() { HerdClient client(cfg2, second); end2 = client.Run(); });
    CHECK(WaitFor([&]() { return second.waiting > 0; }, 10000));
    CHECK("1" == Call("GET", "/status")["clients_total"]);

    // One that is told not to wait ends with the pool exhausted
    std::thread t3([&]() { HerdClient client(cfg3, third); end3 = client.Run(); });
    t3.join();
    CHECK(HERD_END_POOL_EXHAUSTED == end3);
    CHECK(0 == third.waiting);

    second.stop = true;
    t2.join();
    CHECK(HERD_END_CLEAN == end2);
    CHECK(second.waitReason.find("no free account") != std::string::npos);

    first.stop = true;
    t1.join();
}


// Standard section header: blank line, rule, title, rule, blank line
static void Header(const char *title)
{
    const std::string rule(90, '-');
    std::printf("\n%s\n%s\n%s\n\n", rule.c_str(), title, rule.c_str());
}

// Runs one test with a header and a result line, so the runner's own log output in between is
// attributed to the right test. 'expect' says what the runner log is supposed to look like.
static void RunTest(const char *name, void (*test)(), const char *expect)
{
    int failuresBefore = g_failures;
    int checksBefore   = g_checks;

    Header(name);
    std::printf("expected: %s\n\n", expect);
    std::fflush(stdout);

    test();

    if (g_failures == failuresBefore)
        std::printf("[ OK ]   %s (%d checks)\n", name, g_checks - checksBefore);
    else
        std::printf("[ FAIL ] %s (%d of %d checks failed)\n", name, g_failures - failuresBefore, g_checks - checksBefore);

    std::fflush(stdout);
}

int main()
{
    std::string err;

    if (!NetInit(err))
    {
        std::printf("%s\n", err.c_str());
        return 1;
    }

    const char *port = std::getenv("HERD_TEST_PORT");
    ParseHttpUrl(std::string("http://127.0.0.1:") + (port ? port : "18790"), g_url, err);

    RunTest("dummy clients", TestDummyClients,
            "3 clients run, pause and stop; runner finishes with 0 failed");
    RunTest("fill the pool", TestFillPool,
            "4 clients register, the 5th finds no free account (normal end of fill mode); 0 failed");
    RunTest("client cap (--max-clients)", TestFillCap,
            "2 of 4 accounts used, then the limit notice; 0 failed");
    RunTest("runner starts nshtestherd itself as the child", TestSelfChild,
            "a 'child pid=...' line, job finished (exit 0); 0 failed");
    RunTest("HTTP reply framing", TestReplyFraming,
            "no runner output (truncated, chunked and malformed replies are rejected)");
    RunTest("shared client core with custom job hooks (what domlem uses)", TestHerdClientHooks,
            "no runner output (a counted job step runs, stands still while paused, then stop)");
    RunTest("coordinator token", TestClientToken,
            "no runner output (a client without the token is refused and ends, one with it works)");
    RunTest("message over the coordinator's limit", TestLongMessage,
            "no runner output (the message is cut before the character that crosses the limit, the client still stops on command)");
    RunTest("waiting for an account", TestWaitForAccount,
            "no runner output (one waits and reports it, one ends with the pool exhausted)");
    RunTest("custom hooks: failing job, stopped job, failing identity setup", TestHerdClientFailures,
            "no runner output (failed and stopped jobs are reported, the client stays; only the identity setup ends it in error)");
    RunTest("job result kept until the coordinator has it", TestResultNotLost,
            "no runner output (a refused report, then the next job ends: both results are counted)");
    RunTest("program argument placeholders {NSHTEST_...}", TestArgTemplates,
            "no runner output (expansion, old names, literal text, unknown names and {NSHTEST_PASSWORD} refused)");
#ifndef _WIN32
    RunTest("child that ignores SIGTERM", TestStubbornChild,
            "no runner output (the child is killed and reaped)");
    RunTest("external program: success, then failing job", TestExternalProgram,
            "first job ok (exit 0), second job failed (exit 1), the client stays until stop; 0 failed");
    RunTest("external program that cannot be started", TestMissingProgram,
            "the job is reported as failed, the client stays until stop; 0 failed");
    RunTest("placeholders reach the child as arguments", TestTemplatedChild,
            "job finishes (exit 0): the child got load1, tpljob and 1 as arguments; 0 failed");
#endif

    Header("Summary");
    std::printf("%s %d checks, %d failures\n\n", g_failures ? "[ FAIL ]" : "[ OK ]  ", g_checks, g_failures);
    NetCleanup();
    return g_failures ? 1 : 0;
}
