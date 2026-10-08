// httpclient.cpp - minimal blocking HTTP/1.1 client

#include "httpclient.h"
#include "platform.h"

#include <cctype>
#include <cstdlib>
#include <cstring>

namespace
{

const size_t MAX_REPLY_BYTES = 4 * 1024 * 1024;

SocketHandle ConnectTo(const HttpUrl &url, int timeoutSeconds, std::string &err)
{
    struct addrinfo  hints;
    struct addrinfo *res = NULL;

    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (0 != getaddrinfo(url.host.c_str(), std::to_string(url.port).c_str(), &hints, &res) || !res)
    {
        err = "cannot resolve host: " + url.host;
        return INVALID_SOCK;
    }

    SocketHandle s = INVALID_SOCK;

    for (struct addrinfo *ai = res; ai && INVALID_SOCK == s; ai = ai->ai_next)
    {
        SocketHandle candidate = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);

        if (INVALID_SOCK == candidate)
            continue;

        // Non-blocking connect so an unreachable server cannot stall for minutes
        SockSetBlocking(candidate, false);

        bool connected = (0 == connect(candidate, ai->ai_addr, (int)ai->ai_addrlen));

        if (!connected && SockConnectInProgress())
            connected = SockWaitConnected(candidate, timeoutSeconds);

        if (!connected)
        {
            SockClose(candidate);
            continue;
        }

        SockSetBlocking(candidate, true);
        SockSetTimeout(candidate, timeoutSeconds);
        s = candidate;
    }

    freeaddrinfo(res);

    if (INVALID_SOCK == s)
        err = "cannot connect to " + url.host + ":" + std::to_string(url.port);

    return s;
}

} // namespace

bool ParseHttpUrl(const std::string &url, HttpUrl &out, std::string &err)
{
    const std::string scheme = "http://";

    if (url.compare(0, scheme.size(), scheme) != 0)
    {
        err = "server URL must start with http:// (https is not supported)";
        return false;
    }

    std::string rest = url.substr(scheme.size());

    while (!rest.empty() && '/' == rest.back())
        rest.pop_back();

    if (rest.empty() || std::string::npos != rest.find('/'))
    {
        err = "server URL must be http://host[:port] without a path";
        return false;
    }

    size_t colon = rest.rfind(':');

    out.port = 80;

    if (std::string::npos == colon)
        out.host = rest;
    else
    {
        out.host = rest.substr(0, colon);
        std::string port = rest.substr(colon + 1);

        if (port.empty() || port.size() > 5 || port.find_first_not_of("0123456789") != std::string::npos || std::stoi(port) < 1 || std::stoi(port) > 65535)
        {
            err = "invalid port in server URL";
            return false;
        }

        out.port = std::stoi(port);
    }

    if (out.host.empty())
    {
        err = "missing host in server URL";
        return false;
    }

    return true;
}

bool HttpCall(const HttpUrl &url, const std::string &method, const std::string &pathAndQuery, const std::string &formBody, HttpReply &reply, std::string &err,
              int timeoutSeconds, const std::string &bearerToken)
{
    reply = HttpReply();

    SocketHandle s = ConnectTo(url, timeoutSeconds, err);

    if (INVALID_SOCK == s)
        return false;

    std::string request = method + " " + pathAndQuery + " HTTP/1.1\r\n";
    request += "Host: " + url.host + ":" + std::to_string(url.port) + "\r\n";
    request += "Connection: close\r\n";

    if (!bearerToken.empty())
        request += "Authorization: Bearer " + bearerToken + "\r\n";

    if ("POST" == method)
    {
        request += "Content-Type: application/x-www-form-urlencoded\r\n";
        request += "Content-Length: " + std::to_string(formBody.size()) + "\r\n";
    }

    request += "\r\n";

    if ("POST" == method)
        request += formBody;

    size_t sent = 0;

    while (sent < request.size())
    {
        long n = (long)send(s, request.data() + sent, (int)(request.size() - sent), NSH_SEND_FLAGS);

        if (n <= 0)
        {
            err = "send failed: " + SockErrorText();
            SockClose(s);
            return false;
        }

        sent += (size_t)n;
    }

    // The server closes after one response: read until EOF
    std::string raw;
    char        buf[8192];

    for (;;)
    {
        long n = (long)recv(s, buf, sizeof(buf), 0);

        if (n < 0)
        {
            err = SockTimedOut() ? "timeout waiting for reply" : "receive failed: " + SockErrorText();
            SockClose(s);
            return false;
        }

        if (0 == n)
            break;

        raw.append(buf, (size_t)n);

        if (raw.size() > MAX_REPLY_BYTES)
        {
            err = "reply too large";
            SockClose(s);
            return false;
        }
    }

    SockClose(s);

    size_t headerEnd = raw.find("\r\n\r\n");

    if (raw.compare(0, 7, "HTTP/1.") != 0 || raw.size() < 12 || std::string::npos == headerEnd)
    {
        err = "malformed reply";
        return false;
    }

    std::string statusText = raw.substr(9, 3);

    if (statusText.find_first_not_of("0123456789") != std::string::npos || statusText[0] < '1')
    {
        err = "malformed status line";
        return false;
    }

    // Validate framing: a reply cut off by a dropped connection must not look successful
    bool   haveLength    = false;
    size_t contentLength = 0;
    size_t pos           = raw.find("\r\n") + 2;

    while (pos < headerEnd)
    {
        size_t end = raw.find("\r\n", pos);

        if (std::string::npos == end || end > headerEnd)
            end = headerEnd;

        std::string line  = raw.substr(pos, end - pos);
        pos               = end + 2;
        size_t      colon = line.find(':');

        if (std::string::npos == colon)
            continue;

        std::string name = line.substr(0, colon);

        for (char &c : name)
            c = (char)std::tolower((unsigned char)c);

        std::string value = line.substr(colon + 1);
        size_t      b     = value.find_first_not_of(" \t");
        value             = (std::string::npos == b) ? std::string() : value.substr(b);

        if ("transfer-encoding" == name)
        {
            err = "chunked replies are not supported";
            return false;
        }

        if ("content-length" == name)
        {
            if (value.empty() || value.size() > 15 || value.find_first_not_of("0123456789") != std::string::npos)
            {
                err = "invalid Content-Length in reply";
                return false;
            }

            haveLength    = true;
            contentLength = (size_t)std::stoull(value);
        }
    }

    std::string body = raw.substr(headerEnd + 4);

    if (haveLength)
    {
        if (body.size() < contentLength)
        {
            err = "truncated reply (" + std::to_string(body.size()) + " of " + std::to_string(contentLength) + " bytes)";
            return false;
        }

        body.resize(contentLength);
    }

    reply.status = std::atoi(statusText.c_str());
    reply.body   = body;
    return true;
}
