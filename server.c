/*
 * server.c - Phase 4 scheduling server for myshell
 *
 * Thread architecture:
 *   Main thread      — accepts TCP connections, spawns a client_handler
 *                      thread for each one.
 *   Scheduler thread — sleeps until at least one task is queued AND the
 *                      CPU is free, then picks the best task (SRJF + shell
 *                      priority) and signals the owning client thread.
 *   Client threads   — one per connection; submits tasks to the scheduler,
 *                      waits to be woken, executes the task, then loops.
 *
 * Scheduling rules (details in scheduler.c):
 *   - Shell commands always run first (non-preemptive).
 *   - Program tasks (./demo) are preemptable via SIGSTOP / SIGCONT.
 *   - Quantum: 3 s on the first slice, 7 s on every subsequent slice.
 *   - SRJF tiebreak; FCFS when remaining times are equal.
 *   - A task cannot be selected twice in a row unless it is the only one.
 *
 * Networking uses the same send_packet / recv_packet framing from utils.c
 * as Phases 2–3.  An empty (zero-length) packet marks end-of-command so
 * the client knows when to stop reading and prompt for the next command.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <pthread.h>
#include <stdbool.h>

#include "shell.h"
#include "input.h"
#include "server.h"
#include "utils.h"
#include "scheduler.h"

/* ANSI colour codes used in server log output */
#define GRN "\033[32m"
#define YEL "\033[33m"
#define CYN "\033[36m"
#define RED "\033[31m"
#define RST "\033[0m"

/* ── thread-safe logging ─────────────────────────────────────────────────── */

/*
 * log_fp is opened as a dup of the original stdout fd at startup.  This lets
 * us log to the terminal even while a forked child has STDOUT_FILENO pointed
 * at a pipe; the log fd is independent and unaffected by dup2 in children.
 */
static FILE            *log_fp   = NULL;
static pthread_mutex_t  log_lock = PTHREAD_MUTEX_INITIALIZER;

static void slog(const char *fmt, ...)
{
    va_list ap;
    pthread_mutex_lock(&log_lock);   /* prevent interleaved lines from multiple threads */
    va_start(ap, fmt);
    vfprintf(log_fp, fmt, ap);
    va_end(ap);
    fflush(log_fp);                  /* push log line to terminal immediately */
    pthread_mutex_unlock(&log_lock);
}

/* ── client ID allocation ────────────────────────────────────────────────── */

static int             next_tid = 1;
static pthread_mutex_t id_lock  = PTHREAD_MUTEX_INITIALIZER;

/*
 * Assigns a unique, monotonically increasing ID to each connecting client.
 * The lock is needed because multiple client threads call this simultaneously
 * at connection time.
 */
static int alloc_tid(void)
{
    pthread_mutex_lock(&id_lock);
    int id = next_tid++;   /* post-increment: current value returned, then bumped */
    pthread_mutex_unlock(&id_lock);
    return id;
}

/* ── helper: read one newline-terminated line from a file descriptor ─────── */

/*
 * Reads one character at a time to avoid over-reading from a pipe that will
 * later be handed back to a resumed child via SIGCONT.  Buffered wrappers
 * like fgets / getline would consume data beyond the current line into a
 * user-space buffer, corrupting the byte stream for the next scheduling slice
 * (the resumed child would never see those already-consumed bytes again).
 *
 * Returns number of bytes written to buf (including '\n'), 0 on clean EOF,
 * -1 on read error.  buf is always NUL-terminated.
 */
static ssize_t readline_fd(int fd, char *buf, size_t maxlen)
{
    size_t i = 0;
    while (i < maxlen - 1) {     /* leave room for the NUL terminator */
        char    c;
        ssize_t r = read(fd, &c, 1);  /* blocking single-byte read */

        if (r < 0) {
            /* EINTR means a signal interrupted the syscall; just retry.
             * Any other errno is a real error — return -1 to the caller. */
            if (errno == EINTR) continue;
            return -1;
        }

        if (r == 0) {
            /* EOF: the child has closed its write end of the pipe (exited).
             * If we accumulated a partial line, fall through to return it;
             * otherwise return 0 so the caller knows the child is finished. */
            if (i == 0) return 0;
            break;
        }

        buf[i++] = c;
        if (c == '\n') break;   /* complete line collected; stop here */
    }
    buf[i] = '\0';              /* always NUL-terminate for safe string use */
    return (ssize_t)i;
}

/* ── run_builtin_task ────────────────────────────────────────────────────── */

/*
 * Execute a shell / pipeline command to completion (non-preemptive).
 *
 * Forks a child that redirects its stdout and stderr to the write end of a
 * pipe, then calls process_input() — the same parsing + execution path used
 * by the local interactive shell in Phases 1-3.  The parent reads whatever
 * the child writes and forwards it in packets to the client socket.  An empty
 * packet at the end tells the client that the command is finished.
 */
static void run_builtin_task(TaskNode *t)
{

    int pfd[2];
    /* pfd[0] = read end, pfd[1] = write end; child writes, parent reads */
    if (pipe(pfd) < 0) {
        slog("[TASK #%d] ERROR: pipe() failed: %s\n", t->tid, strerror(errno));
        send_packet(t->sockfd, "Internal error: pipe failed\n", 28);
        send_packet(t->sockfd, "", 0);   /* sentinel so client doesn't hang */
        t->state = TASK_DONE;
        return;
    }

    pid_t child = fork();
    if (child < 0) {
        slog("[TASK #%d] ERROR: fork() failed: %s\n", t->tid, strerror(errno));
        close(pfd[0]); close(pfd[1]);
        send_packet(t->sockfd, "Internal error: fork failed\n", 28);
        send_packet(t->sockfd, "", 0);
        t->state = TASK_DONE;
        return;
    }

    if (child == 0) {
        /* ── child process ── */
        close(pfd[0]);                   /* child never reads from the pipe */
        dup2(pfd[1], STDOUT_FILENO);     /* redirect stdout → pipe write end */
        dup2(pfd[1], STDERR_FILENO);     /* redirect stderr → pipe as well so errors reach client */
        close(pfd[1]);                   /* original write fd now redundant after dup2 */

        /* process_input() modifies its argument in-place (strtok, etc.),
         * so we copy cmdline to avoid corrupting the TaskNode string. */
        char cmd_copy[CMD_SIZE];
        strncpy(cmd_copy, t->cmdline, CMD_SIZE - 1);
        cmd_copy[CMD_SIZE - 1] = '\0';   /* guarantee NUL termination */

        /* reuses Phase 1–3 parser + executor pipeline; supports pipes, redirects, etc. */
        process_input(cmd_copy);
        fflush(stdout);   /* flush any remaining output before exit */
        exit(0);
    }

    /* ── parent process: stream child output to client ── */
    close(pfd[1]);   /* parent never writes; closing write end ensures EOF propagates */

    char    buf[4096];
    ssize_t n;
    /* read in chunks (up to 4096 bytes) until child closes its write end */
    while ((n = read(pfd[0], buf, sizeof(buf))) > 0) {
        if (send_packet(t->sockfd, buf, (size_t)n) < 0) {
            slog("[%d] WARN: client disconnected mid-output\n", t->tid);
            break;
        }
        t->bytes_sent += (size_t)n;
    }
    close(pfd[0]);
    waitpid(child, NULL, 0);

    send_packet(t->sockfd, "", 0);
    t->state = TASK_DONE;
}

/* ── run_program_task ────────────────────────────────────────────────────── */

/*
 * Execute one scheduling quantum of a program (./demo) task.
 *
 * First invocation: forks the child, connects its stdout to a pipe.
 * Subsequent invocations: sends SIGCONT to the stopped child.
 *
 * Reads output lines one at a time; each line counts as one second of CPU
 * time.  Stops after `quantum` lines, on a preemption request, or on EOF.
 *
 * After the quantum: sends SIGSTOP to pause the child and returns.
 * After EOF / remaining_secs == 0: reaps the child, sends an empty packet
 * (end-of-command) to the client, and marks the task TASK_DONE.
 */
static void run_program_task(TaskNode *t, int quantum)
{
    /* ── first run: fork and exec the program ── */
    if (!t->launched) {
        int pfd[2];
        if (pipe(pfd) < 0) {
            slog("[TASK #%d] ERROR: pipe() failed: %s\n", t->tid, strerror(errno));
            t->state = TASK_DONE;
            send_packet(t->sockfd, "", 0);  /* sentinel to unblock the client */
            return;
        }

        t->cpid = fork();
        if (t->cpid < 0) {
            slog("[TASK #%d] ERROR: fork() failed: %s\n", t->tid, strerror(errno));
            close(pfd[0]); close(pfd[1]);
            t->state = TASK_DONE;
            send_packet(t->sockfd, "", 0);
            return;
        }

        if (t->cpid == 0) {
            /* ── child process ── */
            close(pfd[0]);                  /* child only writes to the pipe */
            dup2(pfd[1], STDOUT_FILENO);    /* all printf output goes into the pipe */
            close(pfd[1]);                  /* original write fd is now redundant */

            /* build argv[] array by splitting the command line on whitespace */
            char  cmd_copy[CMD_SIZE];
            strncpy(cmd_copy, t->cmdline, CMD_SIZE - 1);
            cmd_copy[CMD_SIZE - 1] = '\0';

            char *argv[64];
            int   argc = 0;
            char *tok  = strtok(cmd_copy, " \t");
            while (tok && argc < 63) {   /* cap at 63 to leave room for NULL terminator */
                argv[argc++] = tok;
                tok = strtok(NULL, " \t");
            }
            argv[argc] = NULL;            /* execvp expects a NULL-terminated array */

            /*
             * If the user typed "demo N" without a leading "./", execvp will not
             * find the binary (it searches PATH, not the current directory).
             * Prepend "./" so it is treated as a local path.
             */
            char  exec_buf[CMD_SIZE];
            char *exec_path = argv[0];
            if (argv[0] && argv[0][0] != '.' && argv[0][0] != '/') {
                snprintf(exec_buf, CMD_SIZE, "./%s", argv[0]);
                exec_path = exec_buf;  /* exec_path points to the fixed path */
                argv[0]   = exec_buf;  /* argv[0] must also be updated so the program sees its own name */
            }

            execvp(exec_path, argv);
            /* execvp only returns on failure */
            fprintf(stderr, "execvp %s: %s\n", exec_path, strerror(errno));
            exit(1);
        }

        /* ── parent: save pipe read end, mark task as running ── */
        close(pfd[1]);              /* parent reads from child; doesn't need write end */
        t->pipe_read_fd = pfd[0];
        t->launched     = true;

    } else {
        /* ── subsequent runs: resume the SIGSTOP'd child ── */
        kill(t->cpid, SIGCONT);
    }

    /* ── consume up to `quantum` lines from the child's output pipe ── */
    int  lines_done = 0;
    char linebuf[1024];

    while (lines_done < quantum && t->remaining_secs > 0) {

        /*
         * Check the preemption flag BEFORE blocking on readline_fd.
         * If a higher-priority task arrived while we were between lines, we
         * should stop immediately rather than block for another second.
         */
        if (t->preempt) break;

        /* readline_fd blocks until the child writes a full line (one second) */
        ssize_t n = readline_fd(t->pipe_read_fd, linebuf, sizeof(linebuf));

        if (n < 0) {
            /* unexpected read error — treat as if the child exited */
            t->remaining_secs = 0;
            break;
        }
        if (n == 0) {
            /* clean EOF: child printed all its lines and exited naturally */
            t->remaining_secs = 0;
            break;
        }

        if (send_packet(t->sockfd, linebuf, (size_t)n) < 0) {
            slog("[%d] WARN: client disconnected, killing child\n", t->tid);
            kill(t->cpid, SIGKILL);
            waitpid(t->cpid, NULL, 0);
            close(t->pipe_read_fd);
            t->state = TASK_DONE;
            return;
        }

        t->remaining_secs--;
        lines_done++;
        t->bytes_sent += (size_t)n;

        /*
         * Check preemption again AFTER receiving a line.  A new task could have
         * arrived and set the flag during the blocking readline_fd call.
         * Checking here keeps the response latency to at most one output line.
         */
        if (t->preempt) break;
    }

    /* record this execution slice (duration = lines_done) for the Gantt chart */
    timeline_record(t->tid, lines_done);

    /* ── decide: task done or just paused ── */
    if (t->remaining_secs <= 0) {
        waitpid(t->cpid, NULL, 0);
        close(t->pipe_read_fd);
        send_packet(t->sockfd, "", 0);
        t->state = TASK_DONE;
    } else {
        kill(t->cpid, SIGSTOP);
        t->preempt = 0;
        slog("[%d]--- " YEL "waiting" RST " (" YEL "%d" RST ")\n",
             t->tid, t->remaining_secs);
    }
}

/* ── scheduler_loop ──────────────────────────────────────────────────────── */

/*
 * Runs in a dedicated thread; picks the next task whenever the CPU is free
 * and the queue is non-empty, then wakes the owning client thread.
 */
static void *scheduler_loop(void *arg)
{
    (void)arg;  /* no argument used */

    /*
     * Acquire sched_mutex once at thread startup and hold it for the thread's
     * entire lifetime.  The only time this thread releases the lock is inside
     * pthread_cond_wait (which atomically sleeps and releases).  This design is
     * safe because the scheduler never performs blocking I/O — it only inspects
     * and modifies in-memory structures, so it never needs to yield the lock for
     * long periods.  Client threads release the lock before doing their own I/O.
     */
    pthread_mutex_lock(&sched_mutex);

    while (1) {
        /*
         * Wait while nothing is ready to run.  Two conditions must both hold
         * before we can dispatch:
         *   1. cpu_busy == false  — no task is currently executing
         *   2. task_queue != NULL — at least one task is waiting
         * pthread_cond_wait re-checks both each time it is signalled.
         */
        while (cpu_busy || !task_queue)
            pthread_cond_wait(&sched_ready, &sched_mutex);

        /* pick the highest-priority ready task using SRJF + anti-starvation */
        TaskNode *chosen = sched_select_next();

        if (!chosen) {
            /*
             * select_next returned NULL: can happen if anti-starvation logic
             * skips every remaining task (e.g., only one task left and it just
             * ran).  Wait for another signal before re-evaluating; the anti-
             * starvation state will have changed by the next wake-up.
             */
            pthread_cond_wait(&sched_ready, &sched_mutex);
            continue;
        }

        cpu_busy          = true;
        running_task      = chosen;
        chosen->state     = TASK_ACTIVE;
        chosen->wake_flag = true;
        pthread_cond_signal(&chosen->wake_cond);
    }

    /* unreachable; included for completeness */
    pthread_mutex_unlock(&sched_mutex);
    return NULL;
}

/* ── client_handler ──────────────────────────────────────────────────────── */

/*
 * One thread per connected client.
 * Receives commands, creates a TaskNode for each, submits it to the
 * scheduler, waits to be dispatched, runs the task (possibly in multiple
 * slices for program tasks), then loops for the next command.
 */
static void *client_handler(void *arg)
{
    int sockfd = (int)(intptr_t)arg;  /* recover socket fd from void* */
    int tid    = alloc_tid();         /* unique ID for this client */

    slog("[%d]<<< client connected\n", tid);

    while (1) {
        char  *cmd     = NULL;
        size_t cmd_len = 0;

        if (recv_packet(sockfd, &cmd, &cmd_len) < 0) {
            break;
        }

        /* strip trailing CR / LF that terminals or network framing may add */
        int len = (int)strlen(cmd);
        while (len > 0 && (cmd[len-1] == '\n' || cmd[len-1] == '\r'))
            cmd[--len] = '\0';

        if (strcmp(cmd, "exit") == 0) {
            free(cmd);
            break;
        }

        slog("[%d]>>> %s\n", tid, cmd);

        /* ── build a TaskNode for this command ── */
        /*
         * TaskNode lives on the stack here, not the heap.  This is safe because
         * this thread blocks (via pthread_cond_wait) until the task reaches
         * TASK_DONE, guaranteeing the node remains in scope for the entire
         * lifetime that the scheduler holds a pointer to it.
         */
        TaskNode t;
        memset(&t, 0, sizeof(t));  /* zero all fields; avoids undefined values */

        t.tid         = tid;
        t.sockfd      = sockfd;
        t.cmdline     = cmd;        /* points into the recv_packet-allocated buffer */
        t.state       = TASK_QUEUED;
        t.launched    = false;      /* child not yet forked */
        t.n_scheduled = 0;          /* tracks how many times this task has been dispatched */
        t.preempt     = 0;          /* preemption flag starts clear */
        t.wake_flag   = false;      /* scheduler sets true when it picks this task */
        pthread_cond_init(&t.wake_cond, NULL);  /* per-task condition variable */

        /*
         * Classify the command as a schedulable program or a shell built-in:
         *   - "./foo" prefix  → program (local binary, launched via execvp)
         *   - "demo [N]"      → program (shorthand accepted for convenience)
         *   - anything else   → shell command (run via process_input, non-preemptive)
         */
        bool is_prog = (strncmp(cmd, "./", 2) == 0 ||
                        (strncmp(cmd, "demo", 4) == 0 &&
                         (cmd[4] == ' ' || cmd[4] == '\0')));

        if (is_prog) {
            t.is_builtin = false;

            const char *sp  = strchr(cmd, ' ');
            t.total_secs    = (sp && *(sp + 1) != '\0') ? atoi(sp + 1) : 10;
            if (t.total_secs <= 0) t.total_secs = 10;

            t.remaining_secs = t.total_secs;
            t.predicted_secs = t.total_secs;
        } else {
            t.is_builtin     = true;
            t.total_secs     = -1;
            t.remaining_secs = -1;
            t.predicted_secs = -1;
        }

        slog("[%d]--- " GRN "created" RST " (" GRN "%d" RST ")\n",
             tid, t.total_secs);

        /* ── enqueue and wait for the scheduler to dispatch this task ── */
        pthread_mutex_lock(&sched_mutex);
        sched_enqueue(&t);   /* adds to the ready list; may trigger preemption of running task */

        /*
         * Outer loop: a program task may execute across many quanta.
         * Each pass through this loop is one scheduling slice.
         * A shell command completes in a single pass (run_builtin_task sets TASK_DONE).
         */
        while (t.state != TASK_DONE) {

            /* wait for the scheduler to signal this specific task's condition variable */
            while (!t.wake_flag)
                pthread_cond_wait(&t.wake_cond, &sched_mutex);

            int quantum = (t.n_scheduled == 0) ? 3 : 7;

            /* log started on first dispatch, running on all subsequent ones */
            if (t.n_scheduled == 0)
                slog("[%d]--- " GRN "started" RST " (" GRN "%d" RST ")\n",
                     tid, t.total_secs);
            else
                slog("[%d]--- " CYN "running" RST " (" CYN "%d" RST ")\n",
                     tid, t.remaining_secs);

            t.n_scheduled++;

            /*
             * Release the global scheduler mutex while executing the task.
             * Holding it here would block:
             *   (a) other client threads from enqueuing new tasks, and
             *   (b) the scheduler thread from seeing those new arrivals.
             * The run_*_task functions do blocking I/O (pipe reads, send_packet),
             * so they MUST run without holding the lock.
             */
            pthread_mutex_unlock(&sched_mutex);

            if (t.is_builtin)
                run_builtin_task(&t);     /* runs to completion; sets TASK_DONE */
            else
                run_program_task(&t, quantum); /* runs one quantum; may pause */

            /* re-acquire lock before touching shared scheduler state */
            pthread_mutex_lock(&sched_mutex);

            /* mark CPU free and wake the scheduler to pick the next task */
            cpu_busy     = false;
            running_task = NULL;
            t.wake_flag  = false;   /* reset so we don't immediately re-execute before being re-dispatched */
            pthread_cond_signal(&sched_ready);  /* notify scheduler thread */
        }

        /* ── task complete: remove from scheduler's linked list ── */
        /*
         * Dequeue happens here, after TASK_DONE, not inside run_program_task.
         * The node stays in the list during execution so the scheduler can see
         * it (e.g., for the anti-starvation last_tid check) and because
         * sched_dequeue must hold the mutex — which we hold here but not inside
         * the run functions.
         */
        sched_dequeue(&t);

        bool queue_empty = (task_queue == NULL);
        pthread_mutex_unlock(&sched_mutex);

        slog("[%d]<<< %zu bytes sent\n", tid, t.bytes_sent);
        slog("[%d]--- " RED "ended" RST " (" RED "%d" RST ")\n",
             tid, t.remaining_secs);

        pthread_cond_destroy(&t.wake_cond);
        free(cmd);

        if (queue_empty)
            timeline_print();
    }

    close(sockfd);
    return NULL;
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    /*
     * Open a separate file descriptor for logging, duplicated from the real
     * stdout before any child processes are forked.  When a child later calls
     * dup2(pipefd, STDOUT_FILENO), it replaces fd 1 in its own address space,
     * but log_fp's underlying fd (a different number) is unaffected.
     */
    int log_fd = dup(STDOUT_FILENO);
    if (log_fd < 0) { perror("dup"); exit(EXIT_FAILURE); }
    log_fp = fdopen(log_fd, "w");
    if (!log_fp) { perror("fdopen"); exit(EXIT_FAILURE); }
    setbuf(log_fp, NULL);   /* unbuffered: each slog() call appears immediately on the terminal */

    /*
     * Ignore SIGPIPE so that send() / write() returns -1 (with errno=EPIPE)
     * instead of killing the whole server when a client closes its connection
     * unexpectedly.  We handle -1 returns from send_packet explicitly.
     */
    signal(SIGPIPE, SIG_IGN);

    sched_init();   /* zero out Gantt-chart list and last_tid before any threads start */

    /* ── create the listening socket ── */
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); exit(EXIT_FAILURE); }

    int yes = 1;
    /*
     * SO_REUSEADDR allows restarting the server immediately after a crash or
     * ^C without waiting for the OS to release the port from TIME_WAIT state.
     */
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(PORT);        /* host-to-network byte order */
    addr.sin_addr.s_addr = INADDR_ANY;         /* accept connections on all interfaces */

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(srv); exit(EXIT_FAILURE);
    }
    /* BACKLOG controls the kernel's accept queue length for pending connections */
    if (listen(srv, BACKLOG) < 0) {
        perror("listen"); close(srv); exit(EXIT_FAILURE);
    }

    slog("--------------------------\n");
    slog("| Hello, Server Started |\n");
    slog("--------------------------\n");

    /* ── start the dedicated scheduler thread ── */
    pthread_t sched_tid;
    if (pthread_create(&sched_tid, NULL, scheduler_loop, NULL) != 0) {
        perror("pthread_create (scheduler)");
        close(srv);
        exit(EXIT_FAILURE);
    }
    /*
     * Detach the scheduler thread so its resources are freed automatically
     * when it exits, without requiring a pthread_join.  We never join it
     * because it runs for the server's entire lifetime.
     */
    pthread_detach(sched_tid);

    /* ── accept loop: one client thread per connection ── */
    while (1) {
        struct sockaddr_in peer;
        socklen_t          peer_len = sizeof(peer);

        /* accept() blocks until a client connects; returns a new fd for that connection */
        int cfd = accept(srv, (struct sockaddr *)&peer, &peer_len);
        if (cfd < 0) { perror("accept"); continue; }  /* non-fatal; try next connection */

        /*
         * Spawn a thread per client so multiple clients run concurrently.
         * The socket fd is passed as the argument (cast through intptr_t to
         * avoid platform-specific pointer-vs-int size issues).
         */
        pthread_t ctid;
        if (pthread_create(&ctid, NULL, client_handler, (void *)(intptr_t)cfd) != 0) {
            perror("pthread_create (client)");
            close(cfd);   /* clean up the fd since no thread will own it */
            continue;
        }
        pthread_detach(ctid);   /* thread cleans itself up when client_handler returns */
    }

    close(srv);
    return 0;
}
