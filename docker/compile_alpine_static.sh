#!/bin/sh
# Builds a fully static nshtestherd binary on Alpine (musl). Meant to run
# inside an Alpine container with g++ and make already installed (see
# ../Dockerfile) -- this script only does the actual compile/link, not
# environment setup, so it works the same whether it's invoked from a
# Dockerfile RUN or interactively inside your own build container.
#
# Usage: docker/compile_alpine_static.sh [output-path]
# (default output path: ./nshtestherd, i.e. the project root)
set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
PROJECT_ROOT="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)"
OUT="${1:-$PROJECT_ROOT/nshtestherd}"

cd "$PROJECT_ROOT"

# Defined once, reused at every compile AND the final link. GCC's LTO docs
# say optimization flags need to be present at the final link step too, not
# only at each individual compile, so one definition avoids drift.
#
# -Os: size, not speed -- the coordinator's requests are tiny.
# -flto: gives --gc-sections whole-program visibility.
# -ffunction-sections -fdata-sections: lets --gc-sections (at the final link)
# drop functions/data nothing references, including unused parts of the
# statically-linked libstdc++.a.
#
# Unlike nshgeoip, NO -fno-exceptions/-fno-rtti: the server and runner catch
# std::exception (for example when a worker thread cannot be started) and the
# standard library throws on out-of-memory and invalid arguments.
CXXFLAGS="-std=c++17 -Wall -Wextra -Os -flto -pthread -ffunction-sections -fdata-sections"

echo "==> compiling nshtestherd object files"
# Derived from src/*.cpp (same source list the Makefile uses) rather than
# hardcoded, so a new source file cannot silently go missing from the static
# build.
OBJS=""
for f in src/*.cpp; do
    OBJS="$OBJS ${f%.cpp}.o"
done

make $OBJS CXXFLAGS="$CXXFLAGS"

echo "==> compiling fortify shim"
# On Alpine, g++/libstdc++ can emit calls to glibc-style _FORTIFY_SOURCE
# wrapper symbols (__printf_chk, __snprintf_chk, __isoc23_strtol, etc.), but
# musl's *static* libc doesn't provide them (only glibc does).
# fortify_shim.cpp provides thin pass-throughs to the real functions. It is
# compiled and linked here rather than folded into the regular src/ build,
# since linking it into a normal glibc build would fail with "multiple
# definition".
g++ $CXXFLAGS -c "$SCRIPT_DIR/fortify_shim.cpp" -o "$SCRIPT_DIR/fortify_shim.o"

echo "==> linking static binary -> $OUT"
# -static: fully static binary, no shared libc/libstdc++ at all.
# -s: strips the symbol table at link time.
# -Wl,--gc-sections: the dead-code removal that -ffunction-sections and
# -fdata-sections made possible.
g++ $CXXFLAGS -static -s -Wl,--gc-sections -o "$OUT" \
    $OBJS \
    "$SCRIPT_DIR/fortify_shim.o" \
    -lssp_nonshared

echo "==> done: $OUT"
file "$OUT" 2>/dev/null || true
