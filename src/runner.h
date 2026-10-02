// runner.h - optional runner mode: N logical clients driven through the public HTTP API
//
// Uses only httpclient + process; it never touches Herd or the server code.

#pragma once

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
    std::vector<std::string> programArgs; // passed unchanged after "--"
    bool                     verbose = false;
};

// Blocks until every client has finished or stop becomes true.
// Returns 0 if no client failed, 1 otherwise.
int RunRunner(const RunnerConfig &config, const std::atomic<bool> &stop);
