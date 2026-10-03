// runner.h - optional runner mode: N logical clients driven through the public HTTP API
//
// Uses only httpclient + process; it never touches Herd or the server code.

#pragma once

#include "process.h"

#include <atomic>
#include <string>
#include <vector>

struct RunnerConfig
{
    std::string              serverUrl = "http://127.0.0.1:8788";
    int                      clients    = 0;     // exact count; 0: fill mode (up to maxClients, or until the pool is booked)
    int                      maxClients = 100;   // fill mode limit
    bool                     fillAll    = false; // fill mode up to 1000 clients (--clients all)
    int                      pollMs    = 2000;
    std::string              program;     // empty: built-in dummy job
    std::vector<std::string> programArgs; // passed after "--"; {NSH_...} placeholders are replaced per client
    bool                     verbose = false;
};

// Placeholders in program arguments: the names of the environment variables the child receives, in braces
// ({NSH_TEST_ID}, {NSH_SHORTNAME}, {NSH_FIRSTNAME}, {NSH_LASTNAME}, {NSH_INTERNETADDRESS}, {NSH_JOB},
// {NSH_COMMAND_ID}, {NSH_SERVER}). A placeholder is replaced inside its argument (no shell, no splitting).
// Everything else is literal text. {NSH_PASSWORD} is refused: it would show up in the process list.

// Expands the placeholders of one argument using values (name, value). False with err for an unknown
// placeholder or {NSH_PASSWORD}.
bool ExpandArgTemplate(const std::string &arg, const EnvList &values, std::string &out, std::string &err);

// Checks all arguments at startup, so a typo fails at once and not when the first child is launched.
bool ValidateArgTemplates(const std::vector<std::string> &args, std::string &err);

// Blocks until every client has finished or stop becomes true.
// Returns 0 if no client failed, 1 otherwise.
int RunRunner(const RunnerConfig &config, const std::atomic<bool> &stop);
