// process.cpp - direct process launch (posix_spawn / CreateProcessW), no shell involved

#include "process.h"

#include <chrono>
#include <thread>

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstring>
#include <cwchar>

namespace
{

std::wstring Widen(const std::string &utf8)
{
    if (utf8.empty())
        return std::wstring();

    int         n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), NULL, 0);
    std::wstring w((size_t)(n > 0 ? n : 0), L'\0');

    if (n > 0)
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), &w[0], n);

    return w;
}

// Standard CommandLineToArgvW quoting rules
std::string QuoteArg(const std::string &a)
{
    if (!a.empty() && std::string::npos == a.find_first_of(" \t\n\v\""))
        return a;

    std::string out = "\"";
    size_t      backslashes = 0;

    for (char c : a)
    {
        if ('\\' == c)
        {
            backslashes++;
            continue;
        }

        if ('"' == c)
        {
            out.append(backslashes * 2 + 1, '\\');
            out += '"';
        }
        else
        {
            out.append(backslashes, '\\');
            out += c;
        }

        backslashes = 0;
    }

    out.append(backslashes * 2, '\\');
    out += '"';
    return out;
}

bool SameName(const std::wstring &entry, const std::wstring &name)
{
    if (entry.size() <= name.size() || L'=' != entry[name.size()])
        return false;

    return 0 == _wcsnicmp(entry.c_str(), name.c_str(), name.size());
}

} // namespace

bool ChildProcess::Start(const std::string &program, const std::vector<std::string> &args, const EnvList &env, std::string &err)
{
    std::string cmd = QuoteArg(program);

    for (const std::string &a : args)
        cmd += " " + QuoteArg(a);

    std::wstring      wcmd = Widen(cmd);
    std::vector<wchar_t> cmdBuf(wcmd.begin(), wcmd.end());
    cmdBuf.push_back(L'\0');

    // Environment block: inherited entries minus overridden names, plus ours
    std::vector<wchar_t> block;
    wchar_t *current = GetEnvironmentStringsW();

    for (const wchar_t *p = current; p && *p; p += std::wcslen(p) + 1)
    {
        std::wstring entry = p;
        bool         overridden = false;

        for (const auto &kv : env)
        {
            if (SameName(entry, Widen(kv.first)))
                overridden = true;
        }

        if (!overridden)
        {
            block.insert(block.end(), entry.begin(), entry.end());
            block.push_back(L'\0');
        }
    }

    if (current)
        FreeEnvironmentStringsW(current);

    for (const auto &kv : env)
    {
        std::wstring entry = Widen(kv.first) + L"=" + Widen(kv.second);
        block.insert(block.end(), entry.begin(), entry.end());
        block.push_back(L'\0');
    }

    block.push_back(L'\0');

    STARTUPINFOW        si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);

    if (!CreateProcessW(NULL, cmdBuf.data(), NULL, NULL, TRUE, CREATE_UNICODE_ENVIRONMENT, block.data(), NULL, &si, &pi))
    {
        err = "cannot start " + program + " (error " + std::to_string(GetLastError()) + ")";
        return false;
    }

    CloseHandle(pi.hThread);
    native_  = (long long)(intptr_t)pi.hProcess;
    pid_     = (long long)pi.dwProcessId;
    running_ = true;
    return true;
}

bool ChildProcess::Finished(int &exitCode)
{
    if (!running_)
        return false;

    HANDLE h = (HANDLE)(intptr_t)native_;

    if (WAIT_OBJECT_0 != WaitForSingleObject(h, 0))
        return false;

    DWORD code = 1;
    GetExitCodeProcess(h, &code);
    CloseHandle(h);
    exitCode = (int)code;
    running_ = false;
    return true;
}

void ChildProcess::Terminate()
{
    Kill(); // no polite stop for console programs on Windows
}

void ChildProcess::Kill()
{
    if (running_)
        TerminateProcess((HANDLE)(intptr_t)native_, 1);
}

long long CurrentProcessId()
{
    return (long long)GetCurrentProcessId();
}

#else

#include <cerrno>
#include <csignal>
#include <cstring>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

bool ChildProcess::Start(const std::string &program, const std::vector<std::string> &args, const EnvList &env, std::string &err)
{
    std::vector<std::string> envStrings;

    for (char **e = environ; e && *e; e++)
    {
        std::string entry = *e;
        bool        overridden = false;

        for (const auto &kv : env)
        {
            if (0 == entry.compare(0, kv.first.size() + 1, kv.first + "="))
                overridden = true;
        }

        if (!overridden)
            envStrings.push_back(entry);
    }

    for (const auto &kv : env)
        envStrings.push_back(kv.first + "=" + kv.second);

    std::vector<char *> envp;

    for (std::string &s : envStrings)
        envp.push_back(&s[0]);

    envp.push_back(NULL);

    std::vector<std::string> argStrings;
    argStrings.push_back(program);
    argStrings.insert(argStrings.end(), args.begin(), args.end());

    std::vector<char *> argv;

    for (std::string &s : argStrings)
        argv.push_back(&s[0]);

    argv.push_back(NULL);

    // The runner ignores SIGPIPE; children must get the default disposition back
    posix_spawnattr_t attr;
    sigset_t          defaults;
    posix_spawnattr_init(&attr);
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGPIPE);
    posix_spawnattr_setsigdefault(&attr, &defaults);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGDEF);

    pid_t pid = 0;
    int   rc  = posix_spawnp(&pid, program.c_str(), NULL, &attr, argv.data(), envp.data());

    posix_spawnattr_destroy(&attr);

    if (0 != rc)
    {
        err = "cannot start " + program + ": " + std::strerror(rc);
        return false;
    }

    native_  = pid;
    pid_     = pid;
    running_ = true;
    return true;
}

bool ChildProcess::Finished(int &exitCode)
{
    if (!running_)
        return false;

    int   status = 0;
    pid_t r      = 0;

    // A signal interrupting waitpid() is not an exit: ask again
    do
    {
        r = waitpid((pid_t)native_, &status, WNOHANG);
    } while (r < 0 && EINTR == errno);

    if (0 == r)
        return false;

    if (r < 0)
        exitCode = 1; // ECHILD: nothing left to wait for
    else if (WIFEXITED(status))
        exitCode = WEXITSTATUS(status);
    else if (WIFSIGNALED(status))
        exitCode = 128 + WTERMSIG(status);
    else
        exitCode = 1;

    running_ = false;
    return true;
}

void ChildProcess::Terminate()
{
    if (running_)
        kill((pid_t)native_, SIGTERM);
}

void ChildProcess::Kill()
{
    if (running_)
        kill((pid_t)native_, SIGKILL);
}

long long CurrentProcessId()
{
    return (long long)getpid();
}

#endif

void ChildProcess::StopAndReap(int graceMs)
{
    if (!running_)
        return;

    int code = 0;

    Terminate();

    for (int waited = 0; waited < graceMs; waited += 50)
    {
        if (Finished(code))
            return;

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // Ignored or blocked the polite stop: force it, then reap so no zombie or orphan is left
    Kill();

    for (int i = 0; i < 100; i++)
    {
        if (Finished(code))
            return;

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

ChildProcess::~ChildProcess()
{
    StopAndReap(5000);
}
