# Makefile for myshell - Phase 1 through Phase 4
#
# Separate compilation: each .c file is compiled to a .o object independently.
# The linker combines the objects into the final executables.
#
# Targets:
#   all      - build myshell, server, client, and demo
#   myshell  - interactive local shell (Phase 1)
#   server   - scheduling TCP server (Phase 4: SRJF + RR, SIGSTOP/SIGCONT)
#   client   - TCP client (streams output until zero-length sentinel packet)
#   demo     - schedulable test program (prints N lines, one per second)
#   clean    - remove all generated object files and executables
#
# Phase 4 notes:
#   server links scheduler.o for the SRJF + RR algorithm and Gantt chart.
#   demo must be compiled as a standalone binary; the server exec()s it as a
#   child process when a client sends "./demo N" or "demo N".

CC      = gcc
CFLAGS  = -Wall -Wextra -pedantic -g
LDFLAGS = -lpthread

# ── shared object files (reused by myshell and server) ───────────────────────

SHARED_OBJS = parser.o executor.o input.o error_handler.o

# ── default: build everything ─────────────────────────────────────────────────

all: myshell server client demo

# ── Phase 1: interactive local shell ──────────────────────────────────────────

myshell: main.o $(SHARED_OBJS)
	$(CC) $(CFLAGS) -o myshell main.o $(SHARED_OBJS)

# ── Phase 4: scheduling server ────────────────────────────────────────────────
# server.c uses the scheduler (SRJF + RR) and the shared shell parser/executor.

server: server.o scheduler.o $(SHARED_OBJS) utils.o
	$(CC) $(CFLAGS) -o server server.o scheduler.o $(SHARED_OBJS) utils.o $(LDFLAGS)

# ── Phase 2/4: remote shell client ────────────────────────────────────────────

client: client.o utils.o
	$(CC) $(CFLAGS) -o client client.o utils.o

# ── Phase 4: schedulable demo program ─────────────────────────────────────────

demo: demo.o
	$(CC) $(CFLAGS) -o demo demo.o

# ── per-file compilation rules ────────────────────────────────────────────────
# Every .c file that uses shell types depends on shell.h so a header change
# triggers a full rebuild of all affected modules.

main.o: main.c shell.h parser.h input.h
	$(CC) $(CFLAGS) -c main.c

server.o: server.c server.h shell.h input.h utils.h scheduler.h
	$(CC) $(CFLAGS) -c server.c

scheduler.o: scheduler.c scheduler.h
	$(CC) $(CFLAGS) -c scheduler.c

client.o: client.c client.h utils.h
	$(CC) $(CFLAGS) -c client.c

demo.o: demo.c
	$(CC) $(CFLAGS) -c demo.c

parser.o: parser.c parser.h shell.h
	$(CC) $(CFLAGS) -c parser.c

executor.o: executor.c executor.h shell.h
	$(CC) $(CFLAGS) -c executor.c

input.o: input.c input.h parser.h executor.h error_handler.h shell.h
	$(CC) $(CFLAGS) -c input.c

error_handler.o: error_handler.c error_handler.h shell.h
	$(CC) $(CFLAGS) -c error_handler.c

utils.o: utils.c utils.h
	$(CC) $(CFLAGS) -c utils.c

# ── convenience targets ───────────────────────────────────────────────────────

run: myshell
	./myshell

run-server: server
	./server

run-client: client
	./client

# ── clean ─────────────────────────────────────────────────────────────────────

clean:
	rm -f main.o server.o scheduler.o client.o demo.o utils.o \
	      $(SHARED_OBJS) myshell server client demo
