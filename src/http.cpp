// http.cpp - small HTTP/1.1 listener

#include "http.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <exception>

namespace
{

typedef std::chrono::steady_clock Clock;

const char *ReasonPhrase(int status)
{
    switch (status)
    {
        case 200: return "OK";
        case 201: return "Created";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 409: return "Conflict";
        case 413: return "Payload Too Large";
        case 501: return "Not Implemented";
        case 503: return "Service Unavailable";
        default: return "Internal Server Error";
    }
}

std::string Lower(std::string s)
{
    for (char &c : s)
        c = (char)std::tolower((unsigned char)c);

    return s;
}

std::string Trim(const std::string &s)
{
    size_t b = s.find_first_not_of(" \t");

    if (std::string::npos == b)
        return std::string();

    size_t e = s.find_last_not_of(" \t");
    return s.substr(b, e - b + 1);
}

bool SendAll(SocketHandle s, const std::string &data)
{
    size_t sent = 0;

    while (sent < data.size())
    {
        long n = (long)send(s, data.data() + sent, (int)std::min<size_t>(data.size() - sent, 1 << 20), NSH_SEND_FLAGS);

        if (n <= 0)
            return false;

        sent += (size_t)n;
    }

    return true;
}

void SendResponse(SocketHandle s, const HttpResponse &resp)
{
    std::string out = "HTTP/1.1 " + std::to_string(resp.status) + " " + ReasonPhrase(resp.status) + "\r\n";
    out += "Content-Type: " + resp.contentType + "\r\n";
    out += "Content-Length: " + std::to_string(resp.body.size()) + "\r\n";

    if (!resp.allow.empty())
        out += "Allow: " + resp.allow + "\r\n";

    out += "Cache-Control: no-store\r\nConnection: close\r\n\r\n";
    out += resp.body;
    SendAll(s, out);
}

HttpResponse ProtocolError(int status, const std::string &message)
{
    HttpResponse r;
    r.status = status;
    r.body   = "error=http_error\nmessage=" + message + "\n";
    return r;
}

// Close without resetting the connection while the client may still be sending
// (early 4xx replies): half-close, then drain briefly.
void LingerClose(SocketHandle s)
{
    shutdown(s, 1); // SD_SEND / SHUT_WR
    SockSetTimeout(s, 1);

    char   buf[4096];
    size_t drained = 0;

    while (drained < 256 * 1024)
    {
        long n = (long)recv(s, buf, sizeof(buf), 0);

        if (n <= 0)
            break;

        drained += (size_t)n;
    }

    SockClose(s);
}

bool ParseContentLength(const std::string &value, size_t &out)
{
    if (value.empty() || value.size() > 15)
        return false;

    size_t n = 0;

    for (char c : value)
    {
        if (c < '0' || c > '9')
            return false;

        n = n * 10 + (size_t)(c - '0');
    }

    out = n;
    return true;
}

} // namespace

HttpServer::HttpServer(const ServerConfig &config, RequestHandler handler)
    : config_(config), handler_(handler), stop_(false)
{
}

bool HttpServer::Start(std::string &err)
{
    listener_ = socket(AF_INET, SOCK_STREAM, 0);

    if (INVALID_SOCK == listener_)
    {
        err = "socket: " + SockErrorText();
        return false;
    }

#ifndef _WIN32
    int one = 1;
    setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#endif

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((unsigned short)config_.port);

    if (1 != inet_pton(AF_INET, config_.bindAddress.c_str(), &addr.sin_addr))
    {
        err = "invalid bind address (IPv4 literal expected): " + config_.bindAddress;
        return false;
    }

    if (0 != bind(listener_, (struct sockaddr *)&addr, sizeof(addr)))
    {
        err = "bind " + config_.bindAddress + ":" + std::to_string(config_.port) + ": " + SockErrorText();
        return false;
    }

    if (0 != listen(listener_, 128))
    {
        err = "listen: " + SockErrorText();
        return false;
    }

    return true;
}

void HttpServer::Stop()
{
    stop_ = true;
}

bool HttpServer::Run()
{
    bool started = true;

    try
    {
        for (int i = 0; i < config_.workerThreads; i++)
            workers_.emplace_back(&HttpServer::WorkerLoop, this);
    }
    catch (const std::exception &e)
    {
        // Out of threads or memory: serving with fewer workers than configured would hide the
        // problem, so shut down cleanly (the cleanup below joins the workers already started)
        std::fprintf(stderr, "Cannot start worker thread %zu of %d: %s\n", workers_.size() + 1, config_.workerThreads, e.what());
        stop_   = true;
        started = false;
    }

    while (!stop_)
    {
        // Wake up twice a second to notice Stop()
        if (!SockWaitReadable(listener_, 500))
            continue;

        SocketHandle client = accept(listener_, NULL, NULL);

        if (INVALID_SOCK == client)
            continue;

        SockSetTimeout(client, config_.timeoutSeconds);

        bool queued = false;
        {
            std::lock_guard<std::mutex> lock(queueMutex_);

            if (queue_.size() < config_.maxQueue)
            {
                queue_.push_back(client);
                queued = true;
            }
        }

        if (queued)
            queueCv_.notify_one();
        else
        {
            SendResponse(client, ProtocolError(503, "server busy"));
            SockClose(client);
        }
    }

    SockClose(listener_);
    listener_ = INVALID_SOCK;

    {
        // Lock so a worker between its predicate check and wait cannot miss the wakeup
        std::lock_guard<std::mutex> lock(queueMutex_);
    }

    queueCv_.notify_all();

    for (std::thread &t : workers_)
        t.join();

    workers_.clear();

    for (SocketHandle s : queue_)
        SockClose(s);

    queue_.clear();
    return started;
}

void HttpServer::WorkerLoop()
{
    for (;;)
    {
        SocketHandle s = INVALID_SOCK;

        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            queueCv_.wait(lock, [this] { return stop_ || !queue_.empty(); });

            if (stop_)
                return;

            s = queue_.front();
            queue_.pop_front();
        }

        Serve(s);
    }
}

void HttpServer::Serve(SocketHandle s)
{
    HttpRequest  req;
    HttpResponse resp;
    int          errStatus = 0;
    std::string  errMessage;

    if (!ReadRequest(s, req, errStatus, errMessage))
    {
        if (0 != errStatus)
            SendResponse(s, ProtocolError(errStatus, errMessage));

        LingerClose(s);
        return;
    }

    try
    {
        resp = handler_(req);
    }
    catch (const std::exception &)
    {
        resp = ProtocolError(500, "internal error");
    }

    SendResponse(s, resp);
    LingerClose(s);
}

bool HttpServer::ReadRequest(SocketHandle s, HttpRequest &req, int &errStatus, std::string &errMessage)
{
    const Clock::time_point deadline = Clock::now() + std::chrono::seconds(config_.timeoutSeconds);

    std::string buf;
    char        tmp[8192];
    size_t      headerEnd = std::string::npos;
    size_t      scanFrom  = 0;

    // Reads more data; returns false after setting the error (or errStatus 0 on plain disconnect).
    auto readMore = [&]() -> bool
    {
        if (Clock::now() > deadline)
        {
            errStatus  = 408;
            errMessage = "request timeout";
            return false;
        }

        long n = (long)recv(s, tmp, sizeof(tmp), 0);

        if (n > 0)
        {
            buf.append(tmp, (size_t)n);
            return true;
        }

        if (n < 0 && SockTimedOut())
        {
            errStatus  = 408;
            errMessage = "request timeout";
        }
        else
            errStatus = 0;

        return false;
    };

    while (std::string::npos == (headerEnd = buf.find("\r\n\r\n", scanFrom)))
    {
        scanFrom = buf.size() > 3 ? buf.size() - 3 : 0;

        if (buf.size() > config_.maxHeaderBytes)
        {
            errStatus  = 413;
            errMessage = "request headers too large";
            return false;
        }

        if (!readMore())
            return false;
    }

    if (headerEnd > config_.maxHeaderBytes)
    {
        errStatus  = 413;
        errMessage = "request headers too large";
        return false;
    }

    std::string head = buf.substr(0, headerEnd);
    std::string rest = buf.substr(headerEnd + 4);

    // Request line
    size_t lineEnd = head.find("\r\n");
    std::string requestLine = head.substr(0, lineEnd);
    size_t sp1 = requestLine.find(' ');
    size_t sp2 = (std::string::npos == sp1) ? std::string::npos : requestLine.find(' ', sp1 + 1);

    if (std::string::npos == sp2 || requestLine.compare(sp2 + 1, 7, "HTTP/1.") != 0)
    {
        errStatus  = 400;
        errMessage = "malformed request line";
        return false;
    }

    req.method       = requestLine.substr(0, sp1);
    std::string target = requestLine.substr(sp1 + 1, sp2 - sp1 - 1);

    if (target.empty() || '/' != target[0])
    {
        errStatus  = 400;
        errMessage = "malformed request target";
        return false;
    }

    size_t q = target.find('?');
    req.path = target.substr(0, q);

    if (std::string::npos != q)
        req.query = target.substr(q + 1);

    // Headers
    bool   haveLength = false;
    size_t contentLength = 0;
    bool   expectContinue = false;

    size_t pos = (std::string::npos == lineEnd) ? head.size() : lineEnd + 2;

    while (pos < head.size())
    {
        size_t end = head.find("\r\n", pos);

        if (std::string::npos == end)
            end = head.size();

        std::string line = head.substr(pos, end - pos);
        pos              = end + 2;

        size_t colon = line.find(':');

        if (std::string::npos == colon)
        {
            errStatus  = 400;
            errMessage = "malformed header";
            return false;
        }

        std::string name  = Lower(line.substr(0, colon));
        std::string value = Trim(line.substr(colon + 1));

        if ("content-length" == name)
        {
            if (haveLength || !ParseContentLength(value, contentLength))
            {
                errStatus  = 400;
                errMessage = "invalid Content-Length";
                return false;
            }

            haveLength = true;
        }
        else if ("transfer-encoding" == name)
        {
            errStatus  = 501;
            errMessage = "Transfer-Encoding is not supported; send Content-Length";
            return false;
        }
        else if ("content-type" == name)
            req.contentType = value;
        else if ("accept" == name)
            req.accept = value;
        else if ("expect" == name)
            expectContinue = ("100-continue" == Lower(value));
    }

    size_t limit = ("/load" == req.path) ? config_.maxCsvBytes : config_.maxBodyBytes;

    if (contentLength > limit)
    {
        errStatus  = 413;
        errMessage = "request body too large (limit " + std::to_string(limit) + " bytes)";
        return false;
    }

    // Body
    buf = rest;

    if (buf.size() < contentLength && expectContinue)
        SendAll(s, "HTTP/1.1 100 Continue\r\n\r\n");

    while (buf.size() < contentLength)
    {
        if (!readMore())
            return false;
    }

    req.body = buf.substr(0, contentLength);
    return true;
}
