// process.h - launch an external program directly (no shell), poll it, stop it

#pragma once

#include <string>
#include <utility>
#include <vector>

typedef std::vector<std::pair<std::string, std::string>> EnvList;

// All strings are UTF-8 (converted to UTF-16 for the Windows wide APIs).
class ChildProcess
{
public:
    ChildProcess() {}
    ~ChildProcess(); // stops and reaps a child that is still running

    ChildProcess(const ChildProcess &)            = delete;
    ChildProcess &operator=(const ChildProcess &) = delete;

    // program is looked up on PATH if it has no directory part. args are passed
    // unchanged as an argument list; env entries are added to (and override) the
    // inherited environment.
    bool Start(const std::string &program, const std::vector<std::string> &args, const EnvList &env, std::string &err);

    bool Running() const { return running_; }

    // Process id (POSIX) or 0 when not started.
    long long Pid() const { return pid_; }

    // Non-blocking. Returns true (once) when the child has exited and has been reaped.
    // Exit code is 128+signal when killed by a signal (POSIX).
    bool Finished(int &exitCode);

    // Polite stop: SIGTERM (POSIX). On Windows there is no polite stop; this is TerminateProcess.
    void Terminate();

    // Forced stop: SIGKILL / TerminateProcess.
    void Kill();

    // Terminate(), wait up to graceMs, then Kill() and reap. Returns when the child is gone
    // (or after a bounded wait if the OS refuses to let it go).
    void StopAndReap(int graceMs);

private:
    bool      running_ = false;
    long long pid_     = 0;
    long long native_  = 0; // pid, or process HANDLE on Windows
};

// Process id of the calling process.
long long CurrentProcessId();
