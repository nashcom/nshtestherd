# nshtestherd - GNU make (Linux, or MinGW/MSYS2 on Windows)
#
#   make          build nshtestherd
#   make test     build and run test_core and test_runner
#   make clean

CXX      ?= g++
CXXFLAGS ?= -O2
CXXFLAGS += -std=c++17 -Wall -Wextra
LDFLAGS  += -pthread

ifeq ($(OS),Windows_NT)
  EXE   = .exe
  LDLIBS += -lws2_32 -lshell32
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

CORE_OBJ   = $(CORE_SRC:.cpp=.o)
SERVER_OBJ = $(SERVER_SRC:.cpp=.o)
RUNNER_OBJ = $(RUNNER_SRC:.cpp=.o)
HDRS       = $(wildcard src/*.h)

# The worker side of the protocol as plain objects, for other programs to link (domlem compiles the sources itself,
# with the Notes toolchain, because objects of another compiler or glibc do not link there):
#   make herdlib      builds src/httpclient.o src/wire.o src/herdclient.o
# No Notes headers, no threads: any C++17 program can link them.
HERDLIB_OBJ = src/httpclient.o src/wire.o src/herdclient.o

.DEFAULT_GOAL := all

herdlib: $(HERDLIB_OBJ)

all: nshtestherd$(EXE)

nshtestherd$(EXE): src/main.o $(SERVER_OBJ) $(RUNNER_OBJ) $(CORE_OBJ)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

test_core$(EXE): tests/test_core.o $(CORE_OBJ)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

test_runner$(EXE): tests/test_runner.o $(SERVER_OBJ) $(RUNNER_OBJ) $(CORE_OBJ)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

test: test_core$(EXE) test_runner$(EXE) nshtestherd$(EXE)
	./test_core$(EXE)
	./test_runner$(EXE)

%.o: %.cpp $(HDRS)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f src/*.o tests/*.o nshtestherd nshtestherd.exe test_core test_core.exe test_runner test_runner.exe

.PHONY: all test clean herdlib
