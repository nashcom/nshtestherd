# nshtestherd - GNU make (Linux, or MinGW/MSYS2 on Windows)
#
#   make          build nshtestherd and nshtestusers (the user CSV tool)
#   make test     build and run test_core, test_runner and test_users
#   make clean

CXX      ?= g++
CXXFLAGS ?= -O2
CXXFLAGS += -std=c++17 -Wall -Wextra
LDFLAGS  += -pthread

ifeq ($(OS),Windows_NT)
  EXE   = .exe
  LDLIBS += -lws2_32 -lshell32
  # nshtestusers takes its random passwords from BCryptGenRandom
  USERS_LDLIBS = -lbcrypt
else
  # Position independent code, so that these objects also link into a PIE or a shared object
  CXXFLAGS += -fPIC
endif

# State, parsing and API routing (no sockets)
CORE_SRC = src/api.cpp src/herd.cpp src/csv.cpp src/wire.cpp
# Listener
SERVER_SRC = src/http.cpp
# Optional runner: HTTP client + process launcher
RUNNER_SRC = src/runner.cpp src/herdclient.cpp src/httpclient.cpp src/process.cpp

# nshtestusers: makes the user CSV (numbered users or random unique names). It uses src/csv.cpp, so the file it writes is
# read by the same parser as the one in the coordinator. No sockets, no threads.
USERS_LIB_SRC = tools/nshtestusers/gen.cpp tools/nshtestusers/random.cpp

CORE_OBJ      = $(CORE_SRC:.cpp=.o)
SERVER_OBJ    = $(SERVER_SRC:.cpp=.o)
RUNNER_OBJ    = $(RUNNER_SRC:.cpp=.o)
USERS_LIB_OBJ = $(USERS_LIB_SRC:.cpp=.o)
HDRS          = $(wildcard src/*.h tools/nshtestusers/*.h)

# The worker side of the protocol as plain objects, for other programs to link (domlem compiles the sources itself,
# with the Notes toolchain, because objects of another compiler or glibc do not link there):
#   make herdlib      builds src/httpclient.o src/wire.o src/herdclient.o
# No Notes headers, no threads: any C++17 program can link them.
HERDLIB_OBJ = src/httpclient.o src/wire.o src/herdclient.o

# What the objects were built with: compiler, target, C library and flags. The stamp file changes only when one of them
# changes, and every object depends on it, so objects from another toolchain or container are rebuilt instead of failing
# to link (PIE relocations, glibc symbol versions).
BUILD_ID := $(shell $(CXX) -dumpfullversion -dumpmachine 2>/dev/null) $(shell ldd --version 2>/dev/null | head -n 1) $(CXX) $(CXXFLAGS)

.DEFAULT_GOAL := all

.build-id: FORCE
	@echo '$(BUILD_ID)' | cmp -s - $@ || echo '$(BUILD_ID)' > $@

herdlib: $(HERDLIB_OBJ)

all: nshtestherd$(EXE) nshtestusers$(EXE)

nshtestherd$(EXE): src/main.o $(SERVER_OBJ) $(RUNNER_OBJ) $(CORE_OBJ)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

nshtestusers$(EXE): tools/nshtestusers/main.o $(USERS_LIB_OBJ) src/csv.o
	$(CXX) $(LDFLAGS) -o $@ $^ $(USERS_LDLIBS)

test_core$(EXE): tests/test_core.o $(CORE_OBJ)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

test_runner$(EXE): tests/test_runner.o $(SERVER_OBJ) $(RUNNER_OBJ) $(CORE_OBJ)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

test_users$(EXE): tests/test_users.o $(USERS_LIB_OBJ) src/csv.o
	$(CXX) $(LDFLAGS) -o $@ $^ $(USERS_LDLIBS)

test: test_core$(EXE) test_runner$(EXE) test_users$(EXE) nshtestherd$(EXE)
	./test_core$(EXE)
	./test_runner$(EXE)
	./test_users$(EXE)

# Every object depends on all headers (coarse, but never stale), on this Makefile and on the toolchain stamp
%.o: %.cpp $(HDRS) Makefile .build-id
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f src/*.o tests/*.o tools/nshtestusers/*.o .build-id nshtestherd nshtestherd.exe nshtestusers nshtestusers.exe \
	      test_core test_core.exe test_runner test_runner.exe test_users test_users.exe test_users_output.csv

FORCE:

.PHONY: all test clean herdlib FORCE
