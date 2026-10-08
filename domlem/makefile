PROGRAM=domlem
TARGET=domlem
SOURCE= $(PROGRAM).cpp
OBJECT = $(PROGRAM).o

CC=g++
CCOPTS=-c -m64 -std=c++17
NOTESDIR=$(Notes_ExecDirectory)
LIBS=-lnotes -lm -lpthread -lc -ldl

INCDIR = $(LOTUS)/notesapi/include
BOOTOBJS = $(LOTUS)/notesapi/lib/linux64/notes0.o $(LOTUS)/notesapi/lib/linux64/notesai0.o
DEFINES = -DGCC3 -DGCC4 -fno-strict-aliasing -DGCC_LBLB_NOT_SUPPORTED -Wformat -Wall -Wcast-align -Wconversion  -DUNIX -DLINUX -DLINUX86 -DND64 -DLINUX64 -DW -DLINUX86_64 -DDTRACE -DPTHREAD_KERNEL -D_REENTRANT -DUSE_THREADSAFE_INTERFACES -D_POSIX_THREAD_SAFE_FUNCTIONS  -DHANDLE_IS_32BITS -DHAS_IOCP -DHAS_BOOL -DHAS_DLOPEN -DUSE_PTHREAD_INTERFACES -DLARGE64_FILES -D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE -DNDUNIX64 -DLONGIS64BIT -DPRODUCTION_VERSION -DOVERRIDEDEBUG -fPIC -Wno-write-strings

# The worker side of the nshtestherd protocol is shared with the generic runner: the sources of the client core in ../src
# (plain C++, no Notes headers). They are compiled here, with this toolchain, into objects of their own (herd_*.o). Objects
# built next to the sources by the other Makefile would come from another compiler or another glibc, and would not link.
HERDDIR  = ..
HERDOBJS = herd_httpclient.o herd_wire.o herd_herdclient.o
HERDHDRS = $(wildcard $(HERDDIR)/src/*.h)

# Helpers shared by the domlem modules (IsNullStr, CopyStr, ErrorText, ...), the agent runner and the load operations
LIBOBJ = lib.o agentrun.o textgen.o mailclient.o dominoload.o

# User registration module (needed for -switch)
REGOBJ = userreg.o

# What the objects were built with: compiler, target, C library and flags. The stamp file changes only when one of them
# changes, and every object depends on it (and on this makefile), so objects from another build container are rebuilt
# instead of failing to link (PIE relocations, glibc symbol versions).
BUILD_ID := $(shell $(CC) -dumpfullversion -dumpmachine 2>/dev/null) $(shell ldd --version 2>/dev/null | head -n 1) $(CC) $(CCOPTS) $(DEFINES)
BUILDDEPS = makefile .build-id

$(TARGET): $(OBJECT) $(LIBOBJ) $(REGOBJ) $(HERDOBJS)
	$(CC) $(OBJECT) $(LIBOBJ) $(REGOBJ) $(HERDOBJS) $(BOOTOBJS) -L$(NOTESDIR) -Wl,-rpath-link,$(NOTESDIR) -no-pie $(LIBS) -o $(TARGET)

.build-id: FORCE
	@echo '$(BUILD_ID)' | cmp -s - $@ || echo '$(BUILD_ID)' > $@

herd_%.o: $(HERDDIR)/src/%.cpp $(HERDHDRS) $(BUILDDEPS)
	$(CC) $(CCOPTS) -O2 -Wall -fPIC -I$(HERDDIR)/src $< -o $@

$(OBJECT): $(SOURCE) lib.h userreg.h dominoload.h agentrun.h mailclient.h textgen.h $(HERDHDRS) $(BUILDDEPS)
	$(CC) $(CCOPTS) $(DEFINES) -I$(INCDIR) -I$(HERDDIR)/src $(SOURCE) -o $(OBJECT)

lib.o: lib.cpp lib.h $(BUILDDEPS)
	$(CC) $(CCOPTS) $(DEFINES) -I$(INCDIR) lib.cpp -o lib.o

userreg.o: userreg.cpp userreg.h lib.h $(BUILDDEPS)
	$(CC) $(CCOPTS) $(DEFINES) -I$(INCDIR) userreg.cpp -o userreg.o

agentrun.o: agentrun.cpp agentrun.h lib.h $(BUILDDEPS)
	$(CC) $(CCOPTS) $(DEFINES) -I$(INCDIR) agentrun.cpp -o agentrun.o

textgen.o: textgen.cpp textgen.h $(BUILDDEPS)
	$(CC) $(CCOPTS) $(DEFINES) -I$(INCDIR) textgen.cpp -o textgen.o

mailclient.o: mailclient.cpp mailclient.h textgen.h lib.h $(BUILDDEPS)
	$(CC) $(CCOPTS) $(DEFINES) -I$(INCDIR) mailclient.cpp -o mailclient.o

dominoload.o: dominoload.cpp dominoload.h agentrun.h mailclient.h textgen.h lib.h $(BUILDDEPS)
	$(CC) $(CCOPTS) $(DEFINES) -I$(INCDIR) dominoload.cpp -o dominoload.o

install: $(TARGET)
	cp -f $(TARGET) /opt/hcl/domino/notes/latest/linux/$(PROGRAM)
	chmod 755  /opt/hcl/domino/notes/latest/linux/$(PROGRAM)

clean:
	rm -f *.o .build-id
	rm -f ./$(TARGET)

all:    $(TARGET)

FORCE:

.PHONY: install clean all FORCE
