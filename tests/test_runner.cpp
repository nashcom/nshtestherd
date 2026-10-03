// test_runner.cpp - runner against a real in-process coordinator over local HTTP
//
// Uses 127.0.0.1:HERD_TEST_PORT (default 18790). If the port is busy the test
// is skipped (exit 0) rather than touching whatever is listening there.
// Dummy-job scenario runs everywhere; the external-program scenarios use /bin/sh
// and are POSIX only.

#include "../src/api.h"
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

    explicit TestServer(int port)
        : server(MakeConfig(port), [this](const HttpRequest &r) { return HandleRequest(herd, r); })
    {
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
    cfg.programArgs  = { "-c", "echo run >> \"$HERD_TEST_COUNT\"; test \"$NSH_SHORTNAME\" = load1 && test \"$NSH_PASSWORD\" = TestPw && test \"$NSH_JOB\" = envjob && test \"$NSH_COMMAND_ID\" = 1 && test \"$NSH_TEST_ID\" = 1" };

    std::atomic<bool> stop(false);
    int               rc = -1;
    std::thread       rt([&]() { rc = RunRunner(cfg, stop); });

    CHECK(WaitFor([]() { return Call("GET", "/status")["clients_total"] == "1"; }, 10000));

    // Successful child: account and job arrive via environment, client returns to idle
    Call("POST", "/command", "test_id=1&command=run&job=envjob");
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

    // A new run command is a new execution; this one fails (NSH_JOB mismatch) -> error, runner exits 1
    Call("POST", "/command", "test_id=1&command=run&job=otherjob");
    CHECK(WaitFor([]() { return Client(1)["state"] == "error"; }, 10000));
    CHECK(Client(1)["message"].find("failed (exit 1)") != std::string::npos);
    rt.join();
    CHECK(1 == rc);

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
    CHECK(WaitFor([]() { return Client(1)["state"] == "error"; }, 10000));
    rt.join();
    CHECK(1 == rc);
}

// The placeholders really arrive as arguments of the child (and the environment is still passed as well)
static void TestTemplatedChild()
{
    TestServer srv(g_url.port);

    if (!srv.Start(2))
        exit(0);

    RunnerConfig cfg = MakeRunner(1);
    cfg.program      = "/bin/sh";
    cfg.programArgs  = { "-c", "test \"$#\" = 3 && test \"$1\" = load1 && test \"$2\" = tpljob && test \"$3\" = 1 && test \"$NSH_SHORTNAME\" = load1",
                         "sh", "{NSH_SHORTNAME}", "{NSH_JOB}", "{NSH_TEST_ID}" };

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

// {NSH_...} placeholders in the program arguments (pure string handling, no processes)
static void TestArgTemplates()
{
    EnvList values = {
        { "NSH_TEST_ID", "7" },
        { "NSH_SHORTNAME", "load000007" },
        { "NSH_JOB", "mail read" },
    };

    std::string out;
    std::string err;

    CHECK(ExpandArgTemplate("--user={NSH_SHORTNAME}", values, out, err) && "--user=load000007" == out);
    CHECK(ExpandArgTemplate("{NSH_TEST_ID}-{NSH_SHORTNAME}-{NSH_TEST_ID}", values, out, err) && "7-load000007-7" == out);
    CHECK(ExpandArgTemplate("no placeholder", values, out, err) && "no placeholder" == out);

    // a value with a space stays inside its single argument
    CHECK(ExpandArgTemplate("{NSH_JOB}", values, out, err) && "mail read" == out);

    // anything that is not a complete {NSH_NAME} stays literal (JSON, shell-like text, lone braces)
    CHECK(ExpandArgTemplate("{\"a\":1}", values, out, err) && "{\"a\":1}" == out);
    CHECK(ExpandArgTemplate("{NSH_ open", values, out, err) && "{NSH_ open" == out);
    CHECK(ExpandArgTemplate("{NSH_lower}", values, out, err) && "{NSH_lower}" == out);
    CHECK(ExpandArgTemplate("$NSH_SHORTNAME", values, out, err) && "$NSH_SHORTNAME" == out); // shell syntax is not ours

    // values are inserted as they are, not expanded a second time
    EnvList tricky = { { "NSH_JOB", "{NSH_TEST_ID}" }, { "NSH_TEST_ID", "7" } };
    CHECK(ExpandArgTemplate("{NSH_JOB}", tricky, out, err) && "{NSH_TEST_ID}" == out);

    // errors
    CHECK(!ExpandArgTemplate("{NSH_UNKNOWN}", values, out, err) && err.find("unknown placeholder") != std::string::npos);
    CHECK(!ExpandArgTemplate("--pw={NSH_PASSWORD}", values, out, err) && err.find("not allowed") != std::string::npos);

    // startup validation knows every documented name, and refuses the rest
    std::vector<std::string> good = { "{NSH_TEST_ID}", "{NSH_FIRSTNAME}", "{NSH_LASTNAME}", "{NSH_SHORTNAME}", "{NSH_INTERNETADDRESS}",
                                      "{NSH_JOB}", "{NSH_COMMAND_ID}", "{NSH_SERVER}", "plain" };
    CHECK(ValidateArgTemplates(good, err));
    CHECK(!ValidateArgTemplates({ "--x", "{NSH_TYPO}" }, err));
    CHECK(!ValidateArgTemplates({ "{NSH_PASSWORD}" }, err));
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
    RunTest("program argument placeholders {NSH_...}", TestArgTemplates,
            "no runner output (expansion, literal text, unknown names and {NSH_PASSWORD} refused)");
#ifndef _WIN32
    RunTest("child that ignores SIGTERM", TestStubbornChild,
            "no runner output (the child is killed and reaped)");
    RunTest("external program: success, then failing job", TestExternalProgram,
            "first job finishes (exit 0); second job exits 1, so '1 failed' below is the INTENDED result");
    RunTest("external program that cannot be started", TestMissingProgram,
            "client ends in error and '1 failed' below is the INTENDED result");
    RunTest("placeholders reach the child as arguments", TestTemplatedChild,
            "job finishes (exit 0): the child got load1, tpljob and 1 as arguments; 0 failed");
#endif

    Header("Summary");
    std::printf("%s %d checks, %d failures\n\n", g_failures ? "[ FAIL ]" : "[ OK ]  ", g_checks, g_failures);
    NetCleanup();
    return g_failures ? 1 : 0;
}
