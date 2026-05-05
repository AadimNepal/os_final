# OS Project — Phase 4: Scheduling Server

---

## Dear Manoj,

Do not panic — everything is here and working. The code compiles cleanly, all three test cases pass, and the server runs correctly. **Your job is to write the report.** Everything below is what you need to understand the codebase well enough to write about it confidently.

---

## What This Project Does

This is a multi-phase remote shell project. By Phase 4 (this repo), the server has a **scheduler** that handles multiple clients concurrently on a single CPU using a combined **SRJF + Round-Robin** algorithm with preemption via `SIGSTOP`/`SIGCONT`.

- Clients connect over TCP and send shell commands or `./demo N` programs
- Shell commands run immediately (highest priority, non-preemptive)
- Demo programs are scheduled, preempted, and resumed based on remaining burst time
- First quantum = 3 seconds, subsequent quanta = 7 seconds
- At the end, a Gantt chart is printed showing the scheduling timeline

---

## How To Build and Run

```bash
make          # builds: myshell, server, client, demo

./server      # start the server (port 9090)
./client      # connect a client (in a separate terminal)
./demo 10     # run demo directly (prints 10 lines, 1 per second)
```

To test with multiple clients, open three terminals and run `./client` in each simultaneously.

---

## File Structure

### Phase 4 New Files (the scheduling stuff)

| File | What it does |
|------|-------------|
| `demo.c` | The schedulable test program. Takes `N` as argument, prints `Demo i/N` once per second for N seconds. The `fflush()` after each print is critical — it pushes each line through the pipe immediately so the server can read it line-by-line. |
| `scheduler.h` | Defines the `TaskNode` struct (one per client command), the `TaskState` enum (`TASK_QUEUED` / `TASK_ACTIVE` / `TASK_DONE`), all global scheduler state (`task_queue`, `running_task`, `cpu_busy`, `sched_mutex`, `sched_ready`), and the public API. |
| `scheduler.c` | Implements the scheduling algorithm. `sched_enqueue()` adds a task and triggers preemption if needed. `sched_select_next()` picks the next task using SRJF with shell-command priority and an anti-starvation rule (same task can't run twice in a row unless it's the only one left). Also manages the Gantt chart via `timeline_record()` and `timeline_print()`. |
| `server.c` | The main server. Three thread types: (1) main thread accepts connections and spawns client threads; (2) scheduler thread picks the next task and wakes the right client thread; (3) client threads submit tasks, wait their turn, execute, and loop. Shell commands use `run_builtin_task()`, demo programs use `run_program_task()` with `SIGSTOP`/`SIGCONT` for preemption. |

### Phases 1–3 Infrastructure (reused, don't need to change)

| File | What it does |
|------|-------------|
| `shell.h` | Shared `Command` and `Pipeline` structs used by the parser and executor. |
| `parser.h / parser.c` | Parses a command string into a `Command` struct — handles pipes, `<`, `>`, `>>`, `2>`, quoted strings. |
| `executor.h / executor.c` | Forks and exec's commands/pipelines with proper file descriptor wiring. |
| `input.h / input.c` | `process_input()` — ties parser and executor together. Called by the server when running a shell command in a child process. |
| `error_handler.h / error_handler.c` | Reports errors and validates pipelines. |
| `utils.h / utils.c` | Network framing: `send_packet()` / `recv_packet()` prefix every message with a 4-byte length so reads are never partial. |
| `client.h / client.c` | TCP client. Reads a command from stdin, sends it to the server, then loops reading response packets until the server sends an empty (zero-length) packet as an end-of-command sentinel. |
| `client.h` | Constants: `SERVER_IP`, `PORT`, `BUF_SIZE`. |
| `server.h` | Constants: `PORT` (9090), `CMD_SIZE`, `OUT_SIZE`, `BACKLOG`. |
| `main.c` | The local interactive shell (Phase 1) — not used by the server, but still builds as `myshell`. |
| `phase1.c` | Old Phase 1 reference — ignore this. |

---

## How the Scheduler Works (for the report)

### Thread model
```
Main thread  →  accept()  →  spawn client_handler thread per connection
Scheduler thread  →  sleeps until cpu_busy=false AND queue non-empty
                   →  calls sched_select_next()
                   →  sets task->wake_flag = true, signals task->wake_cond
Client thread  →  waits on wake_cond
              →  wakes up, drops the mutex, executes task
              →  re-acquires mutex, sets cpu_busy=false, signals scheduler
```

### Scheduling algorithm (`sched_select_next`)
1. **Shell commands first** — any queued shell command is returned immediately
2. **SRJF among programs** — pick the program with the fewest `remaining_secs`
3. **FCFS tiebreak** — if two programs have equal remaining time, whichever arrived first wins (queue order)
4. **Anti-starvation** — the same task cannot be picked twice in a row unless it is the only ready task

### Preemption (`sched_enqueue`)
When a new task arrives while a program is running:
- If the new task is a shell command → preempt immediately
- If the new task is a program with shorter `remaining_secs` → preempt
- Preemption sets `running_task->preempt = 1` (atomic flag); the executing thread checks this between output lines and sends `SIGSTOP` to the child process

### Quantum
- First time a program task is dispatched: **3-second quantum**
- Every subsequent dispatch of the same task: **7-second quantum**
- "1 second" = 1 line of output read from the demo program's pipe

---

## The Three Test Cases (run these for the report)

### Test 1 — 3 clients, all shell commands
Open 3 terminals, each runs `./client` and sends a single shell command (`ls`, `pwd`, `echo hello`). Expected: each client gets its output; no Gantt chart (shell commands excluded from timeline).

### Test 2 — 1 shell command + 2 demo programs
- Client 1: `ls`
- Client 2: `./demo 8`
- Client 3: `./demo 5` (connect ~1 second after Client 2)

Expected: `ls` runs first (shell priority). `./demo 5` preempts `./demo 8` when it arrives (5 < 8 remaining). Gantt chart printed at the end.

### Test 3 — 3 demo programs with different N
- Client 1: `./demo 12`
- Client 2: `./demo 8`
- Client 3: `./demo 5`

Expected: SRJF scheduling — shortest remaining time always runs next (with the anti-starvation rule). Each client eventually gets all its lines. Gantt chart shows interleaved execution.

---

## The `phase4/` Folder — READ THIS

The `phase4/` folder contains **a friend's solution to the same assignment**, which was used purely as a reference to understand the expected architecture. **Do not submit anything from that folder.**

More importantly for you: **`phase4/OS Report Phase 4.pdf`** is a complete example report for this exact assignment. Use it as a structural template — it has all the sections the professor wants:

- Title page
- Architecture / Design
- Implementation Highlights
- Execution Instructions
- Testing (with screenshots)
- Challenges
- Division of Tasks
- References

Write your own version of each section based on **our** implementation (the files in the root of this repo, not the phase4 folder). The architectures are different enough that you'll need to describe ours, not copy theirs — and the professor will definitely notice if the report describes code that doesn't match what's submitted.

---

## Quick Reference — Key Design Decisions to Mention in the Report

1. **`readline_fd()` instead of `getline`** — we read character-by-character from the demo pipe to avoid user-space buffering issues across `SIGSTOP`/`SIGCONT` boundaries.
2. **Scheduler thread holds mutex for its entire lifetime** — it only releases it implicitly via `pthread_cond_wait`. This is simpler and correct because it never does blocking I/O.
3. **`process_input()` for shell commands** — we reuse the Phase 1–3 shell infrastructure instead of writing a new parser, so pipelines, redirections, and all shell features still work over the network.
4. **`log_fp` (dup of stdout) + `log_lock`** — thread-safe logging to a stable file descriptor that isn't affected when child processes `dup2` their stdout to a pipe.
5. **Bug we fixed vs. the reference** — the friend's `get_next_job` counts all queue nodes (including shell commands) for the anti-starvation check, which can incorrectly suppress program re-selection. Our `sched_select_next` counts only program tasks (`n_prog`), which is correct.

---

Good luck Manoj. You got this.
