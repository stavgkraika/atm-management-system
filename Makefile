CC = gcc
CFLAGS = -Wall -Wextra -std=c11
LDFLAGS =

# Detect POSIX-like shells so the build links the libraries needed by:
# - pthread: background transfer notification listener
# - dl:      runtime loading of libsqlite3.so/libsqlite3.dylib
# MinGW/MSYS/Cygwin builds use Windows APIs for those features instead.
UNAME_S := $(shell uname -s 2>/dev/null)
ifneq ($(findstring MINGW,$(UNAME_S)),MINGW)
ifneq ($(findstring MSYS,$(UNAME_S)),MSYS)
ifneq ($(findstring CYGWIN,$(UNAME_S)),CYGWIN)
	LDFLAGS += -pthread -ldl
endif
endif
endif
objects = src/main.o src/auth.o src/features.o src/db.o

# Main executable target. Object files are used so incremental rebuilds stay
# quick when only one C file changes.
atm : $(objects)
	$(CC) $(CFLAGS) -o atm $(objects) $(LDFLAGS)

# Entry point and authentication menu.
src/main.o : src/main.c src/header.h
	$(CC) $(CFLAGS) -c src/main.c -o src/main.o

# Password handling and user lookup.
src/auth.o : src/auth.c src/header.h
	$(CC) $(CFLAGS) -c src/auth.c -o src/auth.o

# Account features, transactions, ownership transfer, and IPC notifications.
src/features.o : src/features.c src/features.h src/header.h src/db.h
	$(CC) $(CFLAGS) -c src/features.c -o src/features.o

# SQLite facade with runtime loading and text-file migration.
src/db.o : src/db.c src/db.h src/header.h
	$(CC) $(CFLAGS) -c src/db.c -o src/db.o

# Windows-friendly cleanup. Missing files are ignored.
clean :
	del /Q $(objects) atm.exe 2>NUL || exit 0
