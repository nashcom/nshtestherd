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
endif

# State, parsing and API routing (no sockets)
CORE_SRC = src/api.cpp src/herd.cpp src/csv.cpp src/wire.cpp
# Listener
SERVER_SRC = src/http.cpp
# Optional runner: HTTP client + process launcher
RUNNER_SRC = src/runner.cpp src/httpclient.cpp src/process.cpp

CORE_OBJ   = $(CORE_SRC:.cpp=.o)
SERVER_OBJ = $(SERVER_SRC:.cpp=.o)
RUNNER_OBJ = $(RUNNER_SRC:.cpp=.o)
HDRS       = $(wildcard src/*.h)

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

.PHONY: all test clean
