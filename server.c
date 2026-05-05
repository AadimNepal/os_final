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
    pthread_mutex_lock(&log_lock);
    va_start(ap, fmt);
    vfprintf(log_fp, fmt, ap);
    va_end(ap);
    fflush(log_fp);
    pthread_mutex_unlock(&log_lock);
}

/* ── client ID allocation ────────────────────────────────────────────────── */

static int             next_tid = 1;
static pthread_mutex_t id_lock  = PTHREAD_MUTEX_INITIALIZER;

static int alloc_tid(void)
{
    pthread_mutex_lock(&id_lock);
    int id = next_tid++;
    pthread_mutex_unlock(&id_lock);
    return id;
}

/* ── helper: read one newline-terminated line from a file descriptor ─────── */

/*
 * Reads one character at a time to avoid over-reading from a pipe that will
 * later be handed back to a resumed child via SIGCONT.  Buffered wrappers
 * like fgets / getline would consume data beyond the current line, corrupting
 * the byte stream for the next scheduling slice.
 *
 * Returns number of bytes written to buf (including '\n'), 0 on clean EOF,
 * -1 on read error.  buf is always NUL-terminated.
 */
static ssize_t readline_fd(int fd, char *buf, size_t maxlen)
{
    size_t i = 0;
    while (i < maxlen - 1) {
        char    c;
        ssize_t r = read(fd, &c, 1);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) {
            /* EOF: flush any partial line collected so far */
            if (i == 0) return 0;
            break;
        }
        buf[i++] = c;
        if (c == '\n') break;
    }
    buf[i] = '\0';
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
    slog("[TASK #%d] START shell command: \"%s\"\n", t->tid, t->cmdline);

    int pfd[2];
    if (pipe(pfd) < 0) {
        slog("[TASK #%d] ERROR: pipe() failed: %s\n", t->tid, strerror(errno));
        send_packet(t->sockfd, "Internal error: pipe failed\n", 28);
        send_packet(t->sockfd, "", 0);
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
        /* child: capture all output through the pipe */
        close(pfd[0]);
        dup2(pfd[1], STDOUT_FILENO);
        dup2(pfd[1], STDERR_FILENO);
        close(pfd[1]);

        char cmd_copy[CMD_SIZE];
        strncpy(cmd_copy, t->cmdline, CMD_SIZE - 1);
        cmd_copy[CMD_SIZE - 1] = '\0';

        process_input(cmd_copy);
        fflush(stdout);
        exit(0);
    }

    /* parent: stream output to client */
    close(pfd[1]);

    char    buf[4096];
    ssize_t n;
    while ((n = read(pfd[0], buf, sizeof(buf))) > 0) {
        if (send_packet(t->sockfd, buf, (size_t)n) < 0) {
            slog("[TASK #%d] WARN: client disconnected mid-output\n", t->tid);
            break;
        }
    }
    close(pfd[0]);
    waitpid(child, NULL, 0);

    /* empty packet signals end-of-command to the client */
    send_packet(t->sockfd, "", 0);

    slog("[TASK #%d] DONE shell command.\n", t->tid);
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
    /* ── first run: fork and exec ── */
    if (!t->launched) {
        int pfd[2];
        if (pipe(pfd) < 0) {
            slog("[TASK #%d] ERROR: pipe() failed: %s\n", t->tid, strerror(errno));
            t->state = TASK_DONE;
            send_packet(t->sockfd, "", 0);
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
            /* child: redirect stdout to pipe, exec the program */
            close(pfd[0]);
            dup2(pfd[1], STDOUT_FILENO);
            close(pfd[1]);

            /* tokenise the command line for execvp */
            char  cmd_copy[CMD_SIZE];
            strncpy(cmd_copy, t->cmdline, CMD_SIZE - 1);
            cmd_copy[CMD_SIZE - 1] = '\0';

            char *argv[64];
            int   argc = 0;
            char *tok  = strtok(cmd_copy, " \t");
            while (tok && argc < 63) {
                argv[argc++] = tok;
                tok = strtok(NULL, " \t");
            }
            argv[argc] = NULL;

            /*
             * If the user typed "demo N" (no leading ./), prepend "./" so
             * execvp finds the binary in the current directory.
             */
            char  exec_buf[CMD_SIZE];
            char *exec_path = argv[0];
            if (argv[0] && argv[0][0] != '.' && argv[0][0] != '/') {
                snprintf(exec_buf, CMD_SIZE, "./%s", argv[0]);
                exec_path = exec_buf;
                argv[0]   = exec_buf;
            }

            execvp(exec_path, argv);
            /* only reached on exec failure */
            fprintf(stderr, "execvp %s: %s\n", exec_path, strerror(errno));
            exit(1);
        }

        /* parent setup */
        close(pfd[1]);
        t->pipe_read_fd = pfd[0];
        t->launched     = true;

        slog("[TASK #%d] START program \"%s\" (total=%d s, quantum=%d s)\n",
             t->tid, t->cmdline, t->total_secs, quantum);

    } else {
        /* resume a SIGSTOP'd child */
        kill(t->cpid, SIGCONT);
        slog("[TASK #%d] RESUME program (remaining=%d s, quantum=%d s)\n",
             t->tid, t->remaining_secs, quantum);
    }

    /* ── consume up to `quantum` lines of output ── */
    int  lines_done = 0;
    char linebuf[1024];

    while (lines_done < quantum && t->remaining_secs > 0) {

        /* check preemption before blocking on read */
        if (t->preempt) break;

        ssize_t n = readline_fd(t->pipe_read_fd, linebuf, sizeof(linebuf));

        if (n < 0) {
            /* read error — treat as completed */
            t->remaining_secs = 0;
            break;
        }
        if (n == 0) {
            /* clean EOF: child finished before remaining_secs hit 0 */
            t->remaining_secs = 0;
            break;
        }

        if (send_packet(t->sockfd, linebuf, (size_t)n) < 0) {
            /* client disconnected; kill child, abort task */
            slog("[TASK #%d] WARN: client disconnected, killing child\n", t->tid);
            kill(t->cpid, SIGKILL);
            waitpid(t->cpid, NULL, 0);
            close(t->pipe_read_fd);
            t->state = TASK_DONE;
            return;
        }

        t->remaining_secs--;
        lines_done++;

        /* check preemption after each delivered line as well */
        if (t->preempt) break;
    }

    /* record this slice in the Gantt chart regardless of how it ended */
    timeline_record(t->tid, lines_done);

    /* ── pause or finish ── */
    if (t->remaining_secs <= 0) {
        /* task complete — reap child and notify client */
        waitpid(t->cpid, NULL, 0);
        close(t->pipe_read_fd);
        send_packet(t->sockfd, "", 0);   /* end-of-command marker */
        slog("[TASK #%d] DONE program (ran %d s this slice).\n",
             t->tid, lines_done);
        t->state = TASK_DONE;
    } else {
        /* quantum expired or preempted — suspend the child */
        kill(t->cpid, SIGSTOP);
        t->preempt = 0;   /* clear flag so next slice starts cleanly */
        slog("[TASK #%d] PAUSE program (ran %d s, remaining=%d s).\n",
             t->tid, lines_done, t->remaining_secs);
    }
}

/* ── scheduler_loop ──────────────────────────────────────────────────────── */

/*
 * Runs in a dedicated thread; picks the next task whenever the CPU is free
 * and the queue is non-empty, then wakes the owning client thread.
 */
static void *scheduler_loop(void *arg)
{
    (void)arg;

    pthread_mutex_lock(&sched_mutex);

    while (1) {
        /* sleep while nothing is ready or the CPU is already occupied */
        while (cpu_busy || !task_queue)
            pthread_cond_wait(&sched_ready, &sched_mutex);

        TaskNode *chosen = sched_select_next();
        if (!chosen) {
            /* no eligible task (e.g., anti-starvation skip eliminated all) */
            pthread_cond_wait(&sched_ready, &sched_mutex);
            continue;
        }

        /* mark CPU as occupied and wake the client thread that owns this task */
        cpu_busy          = true;
        running_task      = chosen;
        chosen->state     = TASK_ACTIVE;
        chosen->wake_flag = true;
        pthread_cond_signal(&chosen->wake_cond);

        slog("[SCHEDULER] Dispatched Task #%d (\"%s\", remaining=%d s)\n",
             chosen->tid, chosen->cmdline, chosen->remaining_secs);
    }

    /* unreachable */
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
    int sockfd = (int)(intptr_t)arg;
    int tid    = alloc_tid();

    slog("[CLIENT #%d] Connected.\n", tid);

    while (1) {
        /* receive the next command from the client */
        char  *cmd     = NULL;
        size_t cmd_len = 0;

        if (recv_packet(sockfd, &cmd, &cmd_len) < 0) {
            slog("[CLIENT #%d] Connection lost.\n", tid);
            break;
        }

        /* strip trailing CR / LF */
        int len = (int)strlen(cmd);
        while (len > 0 && (cmd[len-1] == '\n' || cmd[len-1] == '\r'))
            cmd[--len] = '\0';

        if (strcmp(cmd, "exit") == 0) {
            slog("[CLIENT #%d] Requested disconnect.\n", tid);
            free(cmd);
            break;
        }

        slog("[CLIENT #%d] Received: \"%s\"\n", tid, cmd);

        /* ── classify command and build TaskNode ── */
        TaskNode t;
        memset(&t, 0, sizeof(t));

        t.tid         = tid;
        t.sockfd      = sockfd;
        t.cmdline     = cmd;
        t.state       = TASK_QUEUED;
        t.launched    = false;
        t.n_scheduled = 0;
        t.preempt     = 0;
        t.wake_flag   = false;
        pthread_cond_init(&t.wake_cond, NULL);

        /*
         * A command is a schedulable program if it begins with "./" (local
         * binary) or is the bare name "demo" (for convenience).  Everything
         * else is treated as a shell command (ls, cat, grep pipelines, …).
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
            slog("[CLIENT #%d] Task created as program (burst=%d s).\n",
                 tid, t.total_secs);
        } else {
            t.is_builtin     = true;
            t.total_secs     = -1;
            t.remaining_secs = -1;
            t.predicted_secs = -1;
            slog("[CLIENT #%d] Task created as shell command.\n", tid);
        }

        /* ── submit to scheduler and wait for execution ── */
        pthread_mutex_lock(&sched_mutex);
        sched_enqueue(&t);

        /*
         * Loop until the task is fully done.  A single command may require
         * multiple passes through this loop if it is a program task running
         * across several scheduling quanta.
         */
        while (t.state != TASK_DONE) {
            /* wait for the scheduler to dispatch this task */
            while (!t.wake_flag)
                pthread_cond_wait(&t.wake_cond, &sched_mutex);

            /* first slice gets 3 s, subsequent slices get 7 s */
            int quantum = (t.n_scheduled == 0) ? 3 : 7;
            t.n_scheduled++;

            /*
             * Release the global lock during execution so other client
             * threads can enqueue tasks and the scheduler can see them.
             */
            pthread_mutex_unlock(&sched_mutex);

            if (t.is_builtin)
                run_builtin_task(&t);
            else
                run_program_task(&t, quantum);

            pthread_mutex_lock(&sched_mutex);

            /* release the CPU so the scheduler can pick the next task */
            cpu_busy     = false;
            running_task = NULL;
            t.wake_flag  = false;
            pthread_cond_signal(&sched_ready);
        }

        /* task complete: remove from queue */
        sched_dequeue(&t);
        bool queue_empty = (task_queue == NULL);
        pthread_mutex_unlock(&sched_mutex);

        pthread_cond_destroy(&t.wake_cond);
        free(cmd);

        /* print the Gantt chart once the ready queue drains completely */
        if (queue_empty)
            timeline_print();
    }

    close(sockfd);
    slog("[CLIENT #%d] Disconnected.\n", tid);
    return NULL;
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    /* open a logging descriptor that is independent of STDOUT_FILENO so
     * that forked children's dup2 calls cannot interfere with log output */
    int log_fd = dup(STDOUT_FILENO);
    if (log_fd < 0) { perror("dup"); exit(EXIT_FAILURE); }
    log_fp = fdopen(log_fd, "w");
    if (!log_fp) { perror("fdopen"); exit(EXIT_FAILURE); }
    setbuf(log_fp, NULL);

    /* ignore SIGPIPE so send() / write() returns -1 instead of killing us
     * when a client closes the connection unexpectedly */
    signal(SIGPIPE, SIG_IGN);

    sched_init();

    /* create the listening socket */
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); exit(EXIT_FAILURE); }

    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(srv); exit(EXIT_FAILURE);
    }
    if (listen(srv, BACKLOG) < 0) {
        perror("listen"); close(srv); exit(EXIT_FAILURE);
    }

    slog("[INFO] Server listening on port %d — Phase 4 scheduler active.\n", PORT);

    /* start the dedicated scheduler thread */
    pthread_t sched_tid;
    if (pthread_create(&sched_tid, NULL, scheduler_loop, NULL) != 0) {
        perror("pthread_create (scheduler)");
        close(srv);
        exit(EXIT_FAILURE);
    }
    pthread_detach(sched_tid);

    /* accept loop: one thread per client */
    while (1) {
        struct sockaddr_in peer;
        socklen_t          peer_len = sizeof(peer);
        int cfd = accept(srv, (struct sockaddr *)&peer, &peer_len);
        if (cfd < 0) { perror("accept"); continue; }

        pthread_t tid;
        if (pthread_create(&tid, NULL, client_handler, (void *)(intptr_t)cfd) != 0) {
            perror("pthread_create (client)");
            close(cfd);
            continue;
        }
        pthread_detach(tid);
    }

    close(srv);
    return 0;
}
