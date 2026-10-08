// main.cpp - nshtestherd: test herd coordinator (server mode) and optional runner (--runner)

#include "api.h"
#include "http.h"
#include "platform.h"
#include "process.h"
#include "runner.h"
#include "version.h"
#include "wire.h"

#include <atomic>
#include <cctype>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

#ifdef _WIN32
#include <shellapi.h>
#include <windows.h>
#endif


namespace
{

HttpServer       *g_server = NULL;
std::atomic<bool> g_stop(false);

void OnSignal(int)
{
    g_stop = true;

    if (g_server)
        g_server->Stop();
}

void Usage()
{
    std::printf(
        "nshtestherd %s - test herd coordinator (server mode) and optional runner\n"
        "\n"
        "Server mode (default):\n"
        "  nshtestherd [--csv users.csv] [--port 8788] [--bind 127.0.0.1]\n"
        "\n"
        "  --bind <ipv4>         listen address (default 127.0.0.1; 0.0.0.0 for remote workers)\n"
        "  --port <n>            listen port (default 8788)\n"
        "  --csv <file>          load accounts on startup (FirstName,LastName,Password,Shortname,InternetAddress)\n"
        "  --generate <n>        no CSV: simulate one with n accounts (load000001 ...); mutually exclusive with --csv.\n"
        "                        Default without --csv: 100 accounts. --generate 0: start empty, wait for POST /load\n"
        "  --prefix <name>       generated account prefix (default load)\n"
        "  --password <pw>       generated account password (default TestPassword)\n"
        "  --domain <name>       generated mail domain (default example.com)\n"
        "  --worker-options <s>  options for the workers, returned with every registration (name=value&name, no blanks,\n"
        "                        max 1024); not interpreted here, each worker takes what it knows (domlem: switch)\n"
        "  --threads <n>         worker threads (default 8)\n"
        "  --max-csv-bytes <n>   limit for POST /load bodies (default 16777216)\n"
        "  --timeout <seconds>   socket timeout and request read deadline (default 10)\n"
        "\n"
        "Runner mode (connects to a coordinator over HTTP; may run on any machine):\n"
        "  nshtestherd --runner --server http://host:8788 --clients 100 [--program exe [-- args...]]\n"
        "\n"
        "  --runner              select runner mode\n"
        "  --server <url>        coordinator, http://host[:port] (default http://127.0.0.1:8788)\n"
        "  --clients <n|all>     start exactly n logical clients (max 1000), or 'all' to keep starting clients until\n"
        "                        every free account is booked (max 1000). Default: up to --max-clients, fewer if the\n"
        "                        pool is booked first. One registration and one thread per client.\n"
        "  --max-clients <n>     limit for the default mode (default 100)\n"
        "  --program <exe>       run this program once per client and run command (default: built-in dummy job)\n"
        "  -- <args...>          arguments for every launched program; {NSHTEST_SHORTNAME}, {NSHTEST_TEST_ID}, {NSHTEST_JOB} ...\n"
        "                        are replaced per client (the NSHTEST_* variables are always passed as well)\n"
        "  --poll-seconds <n>    status polling interval (default 2)\n"
        "\n"
        "Smoke test (runs nshtestherd itself as the program; the child prints its pid and NSHTEST_* values):\n"
        "  nshtestherd --runner --clients 2 --program ./nshtestherd -- --child-info\n"
        "\n"
        "Both modes:\n"
        "  --verbose             server: log one line per request: method, path, status, test_id (never bodies)\n"
        "  --version, --help\n"
        "\n"
        "Environment:\n"
        "  NSHTEST_TOKEN         shared secret (16+ printable characters, no blanks). Server: every call except\n"
        "                        GET /health needs 'Authorization: Bearer <token>'. Runner: sent with every call.\n"
        "                        Not an option, so it does not show in the process list.\n",
        NSHTESTHERD_VERSION);
}

bool ReadFile(const std::string &path, std::string &content)
{
    std::ifstream in(path.c_str(), std::ios::binary);

    if (!in)
        return false;

    std::ostringstream ss;
    ss << in.rdbuf();
    content = ss.str();
    return true;
}

bool ParseNumber(const char *text, long long min, long long max, long long &out)
{
    char *end = NULL;
    long long n = std::strtoll(text, &end, 10);

    if (!text[0] || *end || n < min || n > max)
        return false;

    out = n;
    return true;
}

// Accounts generated when neither --csv nor --generate is given: a coordinator works without any option
const size_t DEFAULT_GENERATED_ACCOUNTS = 100;

struct GenerateOptions
{
    size_t      count    = 0;
    bool        given    = false; // --generate given (0: start with an empty pool, wait for POST /load)
    std::string prefix   = "load";
    std::string password = "TestPassword";
    std::string domain   = "example.com";
    bool        custom   = false; // --prefix/--password/--domain given
};

bool IsNameChars(const std::string &s)
{
    if (s.empty())
        return false;

    for (char c : s)
    {
        if (!std::isalnum((unsigned char)c) && !std::strchr("._-", c))
            return false;
    }

    return true;
}

bool IsTokenChars(const std::string &s)
{
    for (char c : s)
    {
        if ((unsigned char)c <= 0x20 || (unsigned char)c > 0x7E)
            return false;
    }

    return true;
}

bool IsDigits(const std::string &s)
{
    return !s.empty() && s.size() <= 10 && std::string::npos == s.find_first_not_of("0123456789");
}

// " test_id=N" for the verbose request log, or "". Taken from the request when it names
// a client, otherwise from the /register response. Only digits are ever logged.
std::string LogTestId(const HttpRequest &req, const HttpResponse &resp)
{
    Form        form;
    std::string err;

    if ("/load" != req.path && ParseForm(req.body.empty() ? req.query : req.body, form, err) && IsDigits(form["test_id"]))
        return " test_id=" + form["test_id"];

    if ("/register" == req.path && resp.status < 300)
    {
        ParseTextFields(resp.body, form);

        if (IsDigits(form["test_id"]))
            return " test_id=" + form["test_id"];

        size_t pos = resp.body.find("\"test_id\":");

        if (std::string::npos != pos)
        {
            std::string digits = resp.body.substr(pos + 10, 10);
            digits             = digits.substr(0, digits.find_first_not_of("0123456789"));

            if (IsDigits(digits))
                return " test_id=" + digits;
        }
    }

    return "";
}

int RunServer(const ServerConfig &config, const std::string &csvPath, const GenerateOptions &gen, bool verbose, const std::string &token,
              const std::string &workerOptions)
{
    std::string err;
    Herd        herd;

    herd.SetWorkerOptions(workerOptions);

    if (!workerOptions.empty())
        std::printf("Worker options for every registration: %s\n", workerOptions.c_str());

    if (!csvPath.empty())
    {
        std::string text;

        if (!ReadFile(csvPath, text))
        {
            std::fprintf(stderr, "Cannot read CSV file: %s\n", csvPath.c_str());
            return 1;
        }

        size_t count = 0;
        Result r     = herd.LoadCsv(text, count);

        if (!r.Ok())
        {
            std::fprintf(stderr, "CSV import failed: %s\n", r.message.c_str());
            return 1;
        }

        std::printf("Loaded %zu accounts from %s\n", count, csvPath.c_str());
    }
    else if (gen.count > 0)
    {
        size_t count = 0;
        Result r     = herd.LoadUsers(GenerateUsers(gen.count, gen.prefix, gen.password, gen.domain), count);

        if (!r.Ok())
        {
            std::fprintf(stderr, "Account generation failed: %s\n", r.message.c_str());
            return 1;
        }

        std::printf("Generated %zu accounts (%s000001 ...)%s\n", count, gen.prefix.c_str(),
                    gen.given ? "" : "; the default without --csv/--generate, POST /load can replace them until the first registration");
    }
    else
        std::printf("No accounts yet (--generate 0): registrations wait until POST /load\n");

    HttpServer server(config, [&](const HttpRequest &req)
    {
        HttpResponse resp = HandleRequest(herd, req, token);

        if (verbose)
            std::fprintf(stderr, "%s %s %d%s\n", req.method.c_str(), req.path.c_str(), resp.status, LogTestId(req, resp).c_str());

        return resp;
    });

    if (!server.Start(err))
    {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }

    g_server = &server;
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);

    std::printf("nshtestherd %s listening on %s:%d, %s (Ctrl+C to stop)\n", NSHTESTHERD_VERSION, config.bindAddress.c_str(), config.port,
                token.empty() ? "no token: anyone who reaches the port may send commands" : "token required (NSHTEST_TOKEN)");
    std::fflush(stdout);

    bool ok = server.Run();

    std::printf("Stopped\n");
    return ok ? 0 : 1;
}

// Demo/test child for the runner: "nshtestherd --child-info" prints its process id and
// the NSHTEST_* values it was started with (never the password), then exits 0.
int ChildInfo()
{
    const char *names[] = { "NSHTEST_TEST_ID", "NSHTEST_SHORTNAME", "NSHTEST_INTERNETADDRESS", "NSHTEST_JOB", "NSHTEST_PARAMS",
                            "NSHTEST_COMMAND_ID", "NSHTEST_SERVER" };

    std::printf("child pid=%lld", CurrentProcessId());

    for (const char *name : names)
    {
        const char *value = std::getenv(name);
        std::printf(" %s=%s", name, value ? value : "(unset)");
    }

    std::printf(" NSHTEST_PASSWORD=%s\n", std::getenv("NSHTEST_PASSWORD") ? "(set)" : "(unset)");
    std::fflush(stdout);
    return 0;
}

#ifdef _WIN32

std::string ToUtf8(const wchar_t *w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);

    if (n <= 1)
        return std::string();

    std::string s((size_t)n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, NULL, NULL);
    return s;
}

#endif

} // namespace

int main(int argc, char **argv)
{
#ifdef _WIN32
    // argv arrives in the ANSI code page. Everything else in this program (accounts from the
    // coordinator, environment values for children) is UTF-8, so rebuild argv from the wide command line.
    std::vector<std::string> utf8Args;
    std::vector<char *>      utf8Argv;
    int                      wargc = 0;
    wchar_t                **wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);

    if (wargv)
    {
        for (int i = 0; i < wargc; i++)
            utf8Args.push_back(ToUtf8(wargv[i]));

        LocalFree(wargv);

        for (std::string &s : utf8Args)
            utf8Argv.push_back(&s[0]);

        utf8Argv.push_back(NULL);
        argc = (int)utf8Args.size();
        argv = utf8Argv.data();
    }
#endif

    if (2 == argc && 0 == std::strcmp(argv[1], "--child-info"))
        return ChildInfo();

    ServerConfig config;
    RunnerConfig runner;
    std::string  csvPath;
    GenerateOptions gen;
    bool         verbose     = false;
    bool         runnerMode  = false;
    bool         serverOpts  = false; // server-only options seen
    bool         runnerOpts  = false; // runner-only options seen
    bool         maxClientsGiven = false;
    std::string  workerOptions;   // --worker-options: handed to every worker with its registration

    for (int i = 1; i < argc; i++)
    {
        std::string arg = argv[i];
        const char *value = (i + 1 < argc) ? argv[i + 1] : NULL;
        long long   n = 0;

        if ("--help" == arg || "-h" == arg)
        {
            Usage();
            return 0;
        }
        else if ("--version" == arg)
        {
            std::printf("nshtestherd %s\n", NSHTESTHERD_VERSION);
            return 0;
        }
        else if ("--verbose" == arg)
            verbose = true;
        else if ("--runner" == arg)
            runnerMode = true;
        else if ("--" == arg)
        {
            runnerOpts = true;

            for (i++; i < argc; i++)
                runner.programArgs.push_back(argv[i]);
        }
        else if (!value)
        {
            std::fprintf(stderr, "Option requires a value: %s\n", arg.c_str());
            return 2;
        }
        else if ("--bind" == arg)
        {
            config.bindAddress = argv[++i];
            serverOpts         = true;
        }
        else if ("--csv" == arg)
        {
            csvPath    = argv[++i];
            serverOpts = true;
        }
        else if ("--prefix" == arg)
        {
            gen.prefix = argv[++i];
            gen.custom = serverOpts = true;
        }
        else if ("--password" == arg)
        {
            gen.password = argv[++i];
            gen.custom   = serverOpts = true;
        }
        else if ("--domain" == arg)
        {
            gen.domain = argv[++i];
            gen.custom = serverOpts = true;
        }
        else if ("--worker-options" == arg)
        {
            workerOptions = argv[++i];
            serverOpts    = true;
        }
        else if ("--server" == arg)
        {
            runner.serverUrl = argv[++i];
            runnerOpts       = true;
        }
        else if ("--program" == arg)
        {
            runner.program = argv[++i];
            runnerOpts     = true;
        }
        else if ("--port" == arg && ParseNumber(value, 1, 65535, n))
        {
            config.port = (int)n;
            serverOpts  = true;
            i++;
        }
        else if ("--threads" == arg && ParseNumber(value, 1, 256, n))
        {
            config.workerThreads = (int)n;
            serverOpts           = true;
            i++;
        }
        else if ("--max-csv-bytes" == arg && ParseNumber(value, 1, 1LL << 31, n))
        {
            config.maxCsvBytes = (size_t)n;
            serverOpts         = true;
            i++;
        }
        else if ("--timeout" == arg && ParseNumber(value, 1, 3600, n))
        {
            config.timeoutSeconds = (int)n;
            serverOpts            = true;
            i++;
        }
        else if ("--generate" == arg && ParseNumber(value, 0, 1000000, n))
        {
            gen.count  = (size_t)n;
            gen.given  = true;
            serverOpts = true;
            i++;
        }
        else if ("--clients" == arg && "all" == std::string(value))
        {
            runner.fillAll = true;
            runnerOpts     = true;
            i++;
        }
        else if ("--clients" == arg && ParseNumber(value, 1, 1000, n))
        {
            runner.clients = (int)n;
            runnerOpts     = true;
            i++;
        }
        else if ("--max-clients" == arg && ParseNumber(value, 1, 1000, n))
        {
            runner.maxClients = (int)n;
            maxClientsGiven   = true;
            runnerOpts        = true;
            i++;
        }
        else if ("--poll-seconds" == arg && ParseNumber(value, 1, 3600, n))
        {
            runner.pollMs = (int)n * 1000;
            runnerOpts    = true;
            i++;
        }
        else
        {
            std::fprintf(stderr, "Invalid option or value: %s\n", arg.c_str());
            Usage();
            return 2;
        }
    }

    if (runnerMode && serverOpts)
    {
        std::fprintf(stderr, "--bind, --port, --csv, --generate, --prefix, --password, --domain, --worker-options, --threads, --max-csv-bytes and --timeout are server options and cannot be used with --runner\n");
        return 2;
    }

    if (!runnerMode && runnerOpts)
    {
        std::fprintf(stderr, "--server, --clients, --max-clients, --program, --poll-seconds and -- are runner options; add --runner\n");
        return 2;
    }

    if (maxClientsGiven && (runner.clients > 0 || runner.fillAll))
    {
        std::fprintf(stderr, "--max-clients only applies when --clients is not given\n");
        return 2;
    }

    if (gen.given && !csvPath.empty())
    {
        std::fprintf(stderr, "--generate and --csv are mutually exclusive\n");
        return 2;
    }

    // No accounts given: generate the default pool (--generate 0 starts empty on purpose)
    if (!runnerMode && csvPath.empty() && !gen.given)
        gen.count = DEFAULT_GENERATED_ACCOUNTS;

    if (gen.custom && (!csvPath.empty() || 0 == gen.count))
    {
        std::fprintf(stderr, "--prefix, --password and --domain apply to generated accounts: not with --csv or --generate 0\n");
        return 2;
    }

    if (gen.count > 0 && (!IsNameChars(gen.prefix) || !IsNameChars(gen.domain) || gen.password.empty()))
    {
        std::fprintf(stderr, "--prefix and --domain may contain only letters, digits and . _ - ; --password must not be empty\n");
        return 2;
    }

    if (!runner.programArgs.empty() && runner.program.empty())
    {
        std::fprintf(stderr, "Arguments after -- require --program\n");
        return 2;
    }

    std::string err;

    if (runnerMode && !ValidateArgTemplates(runner.programArgs, err))
    {
        std::fprintf(stderr, "Invalid program argument: %s\n", err.c_str());
        return 2;
    }

    // The token comes from the environment only: an option would show up in the process list
    const char *envToken = std::getenv("NSHTEST_TOKEN");
    std::string token    = envToken ? envToken : "";

    if (!token.empty() && (token.size() < 16 || token.size() > 256 || !IsTokenChars(token)))
    {
        std::fprintf(stderr, "NSHTEST_TOKEN must be 16-256 printable ASCII characters without blanks\n");
        return 2;
    }

    runner.token = token;

    if (!workerOptions.empty() && (workerOptions.size() > 1024 || !IsTokenChars(workerOptions)))
    {
        std::fprintf(stderr, "--worker-options must be 1-1024 printable ASCII characters without blanks (name=value&name)\n");
        return 2;
    }


    if (!NetInit(err))
    {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }

    int rc = 0;

    if (runnerMode)
    {
        runner.verbose = verbose;
        std::signal(SIGINT, OnSignal);
        std::signal(SIGTERM, OnSignal);
        rc = RunRunner(runner, g_stop);
    }
    else
        rc = RunServer(config, csvPath, gen, verbose, token, workerOptions);

    NetCleanup();
    return rc;
}
