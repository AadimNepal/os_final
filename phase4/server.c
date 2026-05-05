#define DEFAULT_BURST 10
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <stdarg.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <signal.h>
#include "net.h"
#include "utils.h"
#include "scheduler.h"
#include <stdbool.h>

static int g_client_counter = 0;
// Logging helpers
static void log_line_prefixed(const char *tag, const char *prefix, const char *fmt, ...) {
    (void)tag;  // tag now unused on purpose, since phase 4 requires different output format
    va_list ap;
    va_start(ap, fmt);
    // Print prefix, then *directly* the formatted payload.
    // Because all the fmt's start with "<<<", ">>>" or "---",
    // this gives outputs like:
    //   prefix = "[1]", fmt = "<<< client connected"  -> "[1]<<< client connected"
    //   prefix = "(1)", fmt = "--- created (-1)"      -> "(1)--- created (-1)"
    fprintf(stderr, "%s", prefix);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}
#define LOG_INFO(...) do { fprintf(stderr, "[INFO] "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } while(0)

// Re-implementing a simple frame receiver compatible with the client
int recv_frame_str(int fd, char **buf) {
    uint32_t len;
    ssize_t r = readn(fd, &len, 4);

    if (r == 0) {
        // clean EOF: client closed connection
        *buf = NULL;
        return 0;
    }
    if (r < 0) {
        // error on socket
        return -1;
    }

    len = ntohl(len);
    if (len == 0) { *buf = strdup(""); return 0; }

    *buf = malloc(len + 1);
    if (!*buf) return -1;

    r = readn(fd, *buf, len);
    if (r <= 0) {  // short read / error
        free(*buf);
        *buf = NULL;
        return -1;
    }

    (*buf)[len] = 0;
    // Strip newline
    if (len > 0 && (*buf)[len-1] == '\n') (*buf)[len-1] = 0;
    return 0;
}

static int send_frame(int fd, const char *buf, uint32_t len) {
    uint32_t be = htonl(len);
    if (writen(fd, &be, 4) != 4) return -1;
    if (len && writen(fd, buf, len) != (ssize_t)len) return -1;
    return 0;
}

// ---------------------------------------------------------------------------
// EXECUTION LOGIC
// ---------------------------------------------------------------------------

// Runs a shell command (non-preemptive, burst -1)
// Reuses logic from Phase 3 but wrapped for the Job system
void execute_shell_job(Job *job) {
    int out_pfd[2];
    if (pipe(out_pfd) < 0) return;

    job->pid = fork();
    if (job->pid == 0) {
        // Child
        close(out_pfd[0]);
        dup2(out_pfd[1], STDOUT_FILENO);
        dup2(out_pfd[1], STDERR_FILENO);
        close(out_pfd[1]);

        char **tokens = parse_command(job->command);
        Stage *stages; int n; const char *err;
        if (build_pipeline(tokens, &stages, &n, &err) < 0) {
            fprintf(stderr, "%s\n", err);
            exit(1);
        }
        exec_pipeline(stages, n, -1);
        exit(0);
    }

    // Parent
    close(out_pfd[1]);
    char buf[1024];
    ssize_t r;
    while ((r = read(out_pfd[0], buf, sizeof(buf))) > 0) {
        // send output to client
        send_frame(job->socket_fd, buf, (uint32_t)r);

        // log bytes sent
        char prefix[64];
        snprintf(prefix, sizeof(prefix), "[%d]", job->id);
        log_line_prefixed("SENT", prefix, "<<< %zd bytes sent", r);
    }
    
    // Important: Send empty frame to signal "End of Command"
    send_frame(job->socket_fd, NULL, 0);

    close(out_pfd[0]);
    waitpid(job->pid, NULL, 0);
    job->status = JOB_FINISHED;
}

// Runs a demo job (preemptive, creates child, manages SIGSTOP/SIGCONT)
void execute_demo_job(Job *job, int quantum) {
    // Start or Resume
    if (!job->started) {
        int pfd[2];
        if (pipe(pfd) < 0) return;
        
        job->pid = fork();
        if (job->pid == 0) {
            close(pfd[0]);
            // Force line buffering for pipe
            setvbuf(stdout, NULL, _IOLBF, 0); 
            dup2(pfd[1], STDOUT_FILENO);
            close(pfd[1]);
            
            // Execute ./demo N
            char **tokens = parse_command(job->command);
            execvp(tokens[0], tokens);
            exit(1);
        }
        close(pfd[1]);
        job->pipe_fd = pfd[0];
        job->started = true;
        
        char prefix[64]; snprintf(prefix, 64, "(%d)", job->id);
        log_line_prefixed("INFO", prefix, "--- created (%d)", job->total_time);
        log_line_prefixed("INFO", prefix, "--- started (%d)", job->remaining_time);
    } else {
        // Resume
        kill(job->pid, SIGCONT);
        char prefix[64]; snprintf(prefix, 64, "(%d)", job->id);
        log_line_prefixed("INFO", prefix, "--- running (%d)", job->remaining_time);
    }

    int time_consumed = 0;
    FILE *fp = fdopen(job->pipe_fd, "r");
    char *line = NULL; size_t len = 0;

    while (time_consumed < quantum && job->remaining_time > 0) {

        // *** PREEMPTION CHECK ***
        if (job->preempt_requested) {
            // Someone with higher priority arrived; stop after this unit
            break;
        }

        ssize_t read = getline(&line, &len, fp);
        if (read == -1) {
            job->remaining_time = 0; // EOF
            break;
        }

        int rc = send_frame(job->socket_fd, line, (uint32_t)read);
        if (rc < 0) {
            // client disconnected -> kill child, mark job finished, stop running this job
            kill(job->pid, SIGKILL);
            waitpid(job->pid, NULL, 0);
            job->status = JOB_FINISHED;
            job->remaining_time = 0;

            close(job->socket_fd);
            return; // exit execute_demo_job early
        }

        job->remaining_time--;
        time_consumed++;
    }
    free(line);

    // Log chunk sent
    if (time_consumed > 0) {
        char prefix[64];
        snprintf(prefix, sizeof(prefix), "[%d]", job->id);
        log_line_prefixed("SENT", prefix, "<<< %d bytes sent", (int)(time_consumed * 10));
    }

    // Pause or Finish
    if (job->remaining_time > 0) {
        // Either quantum expired OR we were preempted early
        kill(job->pid, SIGSTOP);
        job->preempt_requested = 0;  // clear preemption flag

        char prefix[64]; snprintf(prefix, 64, "(%d)", job->id);
        log_line_prefixed("INFO", prefix, "--- waiting (%d)", job->remaining_time);

        append_timeline(job->id, time_consumed);
    } else {
        // Job finished
        waitpid(job->pid, NULL, 0);
        job->status = JOB_FINISHED;

        // If client already died, this will just fail and we ignore it
        (void)send_frame(job->socket_fd, NULL, 0);

        char prefix[64]; snprintf(prefix, 64, "(%d)", job->id);
        log_line_prefixed("INFO", prefix, "--- ended (%d)", 0);
        append_timeline(job->id, time_consumed);
    }
}

// ---------------------------------------------------------------------------
// THREADS
// ---------------------------------------------------------------------------

// Scheduler Thread
void *scheduler_thread_func(void *arg) {
    while (1) {
        pthread_mutex_lock(&sched_lock);

        // Wait until:
        //  - there is at least one job, AND
        //  - the CPU is not currently being used by some job
        while ((!job_queue) || cpu_busy) {
            pthread_cond_wait(&sched_cond, &sched_lock);
        }

        Job *job = get_next_job();
        if (job) {
            cpu_busy = true;    // CPU is now occupied by this job
            current_job = job;  // <--- remember who owns the CPU
            job->my_turn = true;
            pthread_cond_signal(&job->cond);
        }
        pthread_mutex_unlock(&sched_lock);
    }
    return NULL;
}

// Handles ONE client connection
void *client_thread_func(void *arg) {
    pthread_detach(pthread_self());
    int cfd = (int)(intptr_t)arg;
    int client_id = ++g_client_counter;
    
    char prefix[64]; snprintf(prefix, 64, "[%d]", client_id);
    log_line_prefixed("INFO", prefix, "<<< client connected");

    // Client Loop
    while (1) {
        // Receive Command
        char *cmd = NULL; 
        int rf = recv_frame_str(cfd, &cmd);

        if (rf == -1) {
            // Real error
            log_line_prefixed("ERROR", prefix, "recv_frame_str failed: %s", strerror(errno));
            free(cmd);  // safe even if cmd == NULL
            break;      // will close(cfd) and exit thread
        }
        
        if (cmd == NULL) {
            // Clean disconnect (EOF)
            log_line_prefixed("INFO", prefix, "client disconnected");
            break;
        }
        
        // Log reception
        log_line_prefixed("RECEIVED", prefix, ">>> %s", cmd);

        if (strcmp(cmd, "exit") == 0) {
            free(cmd);
            break;
        }

        // Create Job
        Job j;
        memset(&j, 0, sizeof(j));
        j.preempt_requested = 0;
        j.id = client_id;
        j.socket_fd = cfd;
        j.command = cmd;
        j.started = false;
        j.rounds_run = 0;
        j.status = JOB_WAITING;
        pthread_cond_init(&j.cond, NULL);
        j.my_turn = false;

        // Parse command type
        if (strncmp(cmd, "./demo", 6) == 0 || strncmp(cmd, "demo", 4) == 0) {
            // Known demo program: use N if provided, otherwise default
            j.is_shell_cmd = false;

            char *p = strchr(cmd, ' ');
            if (p) j.total_time = atoi(p + 1);
            else   j.total_time = DEFAULT_BURST;

            j.remaining_time   = j.total_time;
            j.burst_prediction = j.total_time;

        } else if (strncmp(cmd, "./", 2) == 0) {
            // Any other ./program (e.g., ./hello) => unknown burst -> default
            j.is_shell_cmd     = false;
            j.total_time       = DEFAULT_BURST;
            j.remaining_time   = j.total_time;
            j.burst_prediction = j.total_time;

        } else {
            // Plain shell / pipeline commands (pwd, ls, cat foo | grep bar, ... )
            j.is_shell_cmd     = true;
            j.total_time       = -1;
            j.remaining_time   = -1;
            j.burst_prediction = -1;  // "infinite priority" for scheduling
        }

        // Submit to Scheduler
        pthread_mutex_lock(&sched_lock);
        add_job(&j);
        
        // If it's a shell cmd (burst -1), log creation immediately
        if (j.is_shell_cmd) {
             log_line_prefixed("INFO", prefix, "--- created (-1)");
             // Note: It will start when scheduler picks it
        }

        pthread_cond_signal(&sched_cond); // Notify scheduler
        
        // Wait for Execution
        while (j.status != JOB_FINISHED) {
            // Wait for turn
            while (!j.my_turn) {
                pthread_cond_wait(&j.cond, &sched_lock);
            }
            // "I have the lock and it's my turn"
            if (j.is_shell_cmd) {
                log_line_prefixed("INFO", prefix, "--- started (-1)");
                // Execute fully
                pthread_mutex_unlock(&sched_lock); // Release lock during exec
                execute_shell_job(&j);
                pthread_mutex_lock(&sched_lock);   // Reacquire to update state
                
                log_line_prefixed("INFO", prefix, "--- ended (-1)");
                // IMPORTANT: shell commands are NOT part of the Gantt diagram
                // so we do NOT call append_timeline() here.
            } else {
                // Program Execution
                int quantum = (j.rounds_run == 0) ? 3 : 7;
                j.rounds_run++;
                
                // Drop the global scheduler lock while running this quantum,
                // so other clients can enqueue jobs and the scheduler can see them.
                pthread_mutex_unlock(&sched_lock);
                execute_demo_job(&j, quantum);
                pthread_mutex_lock(&sched_lock);
            }
            
            cpu_busy = false;  // CPU is now free for someone else
            current_job = NULL;
            j.my_turn = false; // Yield back to scheduler
            pthread_cond_signal(&sched_cond);  // wake scheduler to pick next job
        }
        
        remove_job(&j);
        pthread_mutex_unlock(&sched_lock);
        
        pthread_cond_destroy(&j.cond);
        free(cmd);
        
        // If queue empty, print timeline
        if (job_queue == NULL) {
             print_timeline();
        }
    }

    close(cfd);
    return NULL;
}

int main(int argc, char **argv) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;  // ignore SIGPIPE globally (this is useful so that the server doesn't crash when client does CTRL+C)
    sigaction(SIGPIPE, &sa, NULL);

    uint16_t port = 5050;
    if (argc > 1) port = atoi(argv[1]);
    
    int lfd = tcp_listen(port);
    if (lfd < 0) return 1;
    
    // UI Header
    printf("\n-------------------------\n");
    printf("| Hello, Server Started |\n");
    printf("-------------------------\n\n");
    
    scheduler_init();

    // Spawn Scheduler
    pthread_t stid;
    pthread_create(&stid, NULL, scheduler_thread_func, NULL);

    while (1) {
        struct sockaddr_in peer; socklen_t len = sizeof(peer);
        int cfd = accept(lfd, (struct sockaddr*)&peer, &len);
        if (cfd < 0) continue;
        
        pthread_t tid;
        pthread_create(&tid, NULL, client_thread_func, (void*)(intptr_t)cfd);
    }
    return 0;
}