// Alpine/musl static-build-only compatibility shim -- NOT part of the
// regular src/ build (glibc already provides these symbols natively, so
// linking this into a normal Ubuntu/glibc build would fail with
// "multiple definition"; it's compiled and linked only by
// docker/compile_alpine_static.sh).
//
// On Alpine, g++'s libstdc++ headers can emit calls to glibc-style
// _FORTIFY_SOURCE wrapper symbols (__printf_chk, __snprintf_chk, etc.), and
// the C23-renamed strto* entry points (__isoc23_strtol, ...) -- but musl's
// *static* libc archive doesn't provide any of these, only glibc does. These
// are thin pass-throughs to the real (non-"_chk") functions: we don't rely
// on _FORTIFY_SOURCE's extra runtime bounds-checking for correctness anywhere
// in nshtestherd (every buffer/length is already explicitly sized and checked
// in src/), so dropping that extra check changes nothing observable, it just
// satisfies the linker.
//
// The first four are the same set nshgeoip needs. The __isoc23_strtoll /
// __isoc23_strtoull / __vsnprintf_chk entries are added because nshtestherd
// uses strtoll, std::stoull and std::to_string (vsnprintf); if the static
// link on Alpine reports further undefined __*_chk or __isoc23_* symbols,
// add the matching pass-through here.

#include <cstdarg>
#include <cstdio>
#include <cstdlib>

extern "C" {

int __printf_chk(int /*flag*/, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int ret = vprintf(format, ap);
    va_end(ap);
    return ret;
}

int __fprintf_chk(FILE *stream, int /*flag*/, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int ret = vfprintf(stream, format, ap);
    va_end(ap);
    return ret;
}

int __snprintf_chk(char *s, size_t maxlen, int /*flag*/, size_t /*slen*/, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int ret = vsnprintf(s, maxlen, format, ap);
    va_end(ap);
    return ret;
}

int __vsnprintf_chk(char *s, size_t maxlen, int /*flag*/, size_t /*slen*/, const char *format, va_list ap) {
    return vsnprintf(s, maxlen, format, ap);
}

long __isoc23_strtol(const char *nptr, char **endptr, int base) {
    return strtol(nptr, endptr, base);
}

long long __isoc23_strtoll(const char *nptr, char **endptr, int base) {
    return strtoll(nptr, endptr, base);
}

unsigned long long __isoc23_strtoull(const char *nptr, char **endptr, int base) {
    return strtoull(nptr, endptr, base);
}

} // extern "C"
