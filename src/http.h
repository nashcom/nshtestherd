// http.h - small HTTP/1.1 listener: one request per connection, Connection: close
//
// A fixed pool of worker threads serves accepted connections from a bounded
// queue. No keep-alive, no chunked requests, no TLS.

#pragma once

#include "platform.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct HttpRequest
{
    std::string method;
    std::string path;
    std::string query; // raw, without '?'
    std::string contentType;
    std::string accept;
    std::string body;
};

struct HttpResponse
{
    int         status      = 200;
    std::string contentType = "text/plain; charset=utf-8";
    std::string body;
    std::string allow; // set on 405
};

typedef std::function<HttpResponse(const HttpRequest &)> RequestHandler;

struct ServerConfig
{
    std::string bindAddress     = "127.0.0.1"; // IPv4 literal
    int         port            = 8788;
    int         workerThreads   = 8;
    size_t      maxQueue        = 64;          // accepted but not yet served
    size_t      maxHeaderBytes  = 16 * 1024;
    size_t      maxBodyBytes    = 64 * 1024;
    size_t      maxCsvBytes     = 16 * 1024 * 1024; // POST /load
    int         timeoutSeconds  = 10;          // socket timeout and total request read deadline
};

class HttpServer
{
public:
    HttpServer(const ServerConfig &config, RequestHandler handler);

    bool Start(std::string &err); // bind + listen
    bool Run();                   // blocks until Stop(); false if the worker threads could not be started
    void Stop();                  // safe from a signal handler

private:
    void WorkerLoop();
    void Serve(SocketHandle s);
    bool ReadRequest(SocketHandle s, HttpRequest &req, int &errStatus, std::string &errMessage);

    ServerConfig            config_;
    RequestHandler          handler_;
    SocketHandle            listener_ = INVALID_SOCK;
    std::atomic<bool>       stop_;
    std::mutex              queueMutex_;
    std::condition_variable queueCv_;
    std::deque<SocketHandle> queue_;
    std::vector<std::thread> workers_;
};
