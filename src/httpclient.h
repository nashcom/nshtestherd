// httpclient.h - minimal blocking HTTP/1.1 client for the runner (http only, one request per connection)

#pragma once

#include <string>

struct HttpUrl
{
    std::string host;
    int         port = 80;
};

// Accepts http://host[:port][/]. https is not supported.
bool ParseHttpUrl(const std::string &url, HttpUrl &out, std::string &err);

struct HttpReply
{
    int         status = 0;
    std::string body;
};

// formBody is sent as application/x-www-form-urlencoded (POST only).
// Connect and I/O are bounded by timeoutSeconds. Returns false on transport errors.
bool HttpCall(const HttpUrl &url, const std::string &method, const std::string &pathAndQuery, const std::string &formBody, HttpReply &reply, std::string &err, int timeoutSeconds = 10);
