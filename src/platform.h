// platform.h - minimal socket portability layer (Windows / POSIX)
//
// Only what the HTTP listener needs: init, close, timeouts, error text.

#pragma once

#include <string>

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

typedef SOCKET SocketHandle;
static const SocketHandle INVALID_SOCK = INVALID_SOCKET;
#define NSH_SEND_FLAGS 0

inline bool NetInit(std::string &err)
{
    WSADATA wsa;

    if (0 != WSAStartup(MAKEWORD(2, 2), &wsa))
    {
        err = "WSAStartup failed";
        return false;
    }

    return true;
}

inline void NetCleanup()
{
    WSACleanup();
}

inline void SockClose(SocketHandle s)
{
    closesocket(s);
}

inline void SockSetTimeout(SocketHandle s, int seconds)
{
    DWORD ms = (DWORD)seconds * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&ms, sizeof(ms));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&ms, sizeof(ms));
}

inline bool SockTimedOut()
{
    int e = WSAGetLastError();
    return (WSAETIMEDOUT == e) || (WSAEWOULDBLOCK == e);
}

inline std::string SockErrorText()
{
    return "winsock error " + std::to_string(WSAGetLastError());
}

inline void SockSetBlocking(SocketHandle s, bool blocking)
{
    u_long mode = blocking ? 0 : 1;
    ioctlsocket(s, FIONBIO, &mode);
}

inline bool SockConnectInProgress()
{
    return WSAEWOULDBLOCK == WSAGetLastError();
}

#else

#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

typedef int SocketHandle;
static const SocketHandle INVALID_SOCK = -1;

#ifdef MSG_NOSIGNAL
#define NSH_SEND_FLAGS MSG_NOSIGNAL
#else
#define NSH_SEND_FLAGS 0
#endif

inline bool NetInit(std::string &)
{
    signal(SIGPIPE, SIG_IGN);
    return true;
}

inline void NetCleanup()
{
}

inline void SockClose(SocketHandle s)
{
    close(s);
}

inline void SockSetTimeout(SocketHandle s, int seconds)
{
    struct timeval tv;
    tv.tv_sec  = seconds;
    tv.tv_usec = 0;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

inline bool SockTimedOut()
{
    return (EAGAIN == errno) || (EWOULDBLOCK == errno);
}

inline std::string SockErrorText()
{
    return std::strerror(errno);
}

inline void SockSetBlocking(SocketHandle s, bool blocking)
{
    int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK));
}

inline bool SockConnectInProgress()
{
    return EINPROGRESS == errno;
}

#endif
