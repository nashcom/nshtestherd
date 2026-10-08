// api.h - HTTP routing and request validation on top of Herd (no sockets here)

#pragma once

#include "herd.h"
#include "http.h"

// token: when not empty, every request except GET /health needs "Authorization: Bearer <token>" (else 401)
HttpResponse HandleRequest(Herd &herd, const HttpRequest &req, const std::string &token = std::string());
