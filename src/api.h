// api.h - HTTP routing and request validation on top of Herd (no sockets here)

#pragma once

#include "herd.h"
#include "http.h"

HttpResponse HandleRequest(Herd &herd, const HttpRequest &req);
