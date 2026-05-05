/*
 * executor.c - command execution for myshell
 *
 * Two public functions:
 *   execute_single_command - fork/exec one command with optional redirections
 *   execute_pipeline       - connect N commands through kernel pipes
 *
 * apply_redirections() is shared by both so the open/dup2/close logic
 * lives in exactly one place.  run_pipeline_stage() encapsulates everything
 * a child process needs to do for one stage of a pipeline, keeping the
 * parent-side loop in execute_pipeline() clean and short.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>

#include "executor.h"

/* ── apply_redirections ──────────────────────────────────────────────────── */

/*
 * Redirect stdin/stdout/stderr for the calling process according to the
 * file names stored in *cmd.  Called from a child process only; exits on
 * any open() failure so the caller never has to check a return value.
 */
static void apply_redirections(const Command *cmd) {
    if (cmd->input_file) {
        int fd = open(cmd->input_file, O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "[ERROR] File not found: \"%s\"\n", cmd->input_file);
            exit(1);
        }
        dup2(fd, STDIN_FILENO);
        close(fd);
    }

    if (cmd->output_file) {
        int flags = O_WRONLY | O_CREAT | (cmd->append_mode ? O_APPEND : O_TRUNC);
        int fd = open(cmd->output_file, flags, 0644);
        if (fd < 0) {
            fprintf(stderr, "[ERROR] Failed to open output file: \"%s\"\n", cmd->output_file);
            exit(1);
        }
        dup2(fd, STDOUT_FILENO);
        close(fd);
    }

    if (cmd->error_file) {
        int fd = open(cmd->error_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            fprintf(stderr, "[ERROR] Failed to open error file: \"%s\"\n", cmd->error_file);
            exit(1);
        }
        dup2(fd, STDERR_FILENO);
        close(fd);
    }
}

/* ── execute_single_command ──────────────────────────────────────────────── */

void execute_single_command(Command *cmd) {
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return;
    }

    if (pid == 0) {
        signal(SIGPIPE, SIG_DFL);
        apply_redirections(cmd);
        execvp(cmd->args[0], cmd->args);
        fprintf(stderr, "[ERROR] Command not found: \"%s\"\n", cmd->args[0]);
        exit(1);
    }

    int status;
    waitpid(pid, &status, 0);
}

/* ── pipeline internals ──────────────────────────────────────────────────── */

/*
 * run_pipeline_stage - child-side setup and exec for one pipeline stage.
 *
 * Wires stdin/stdout to the appropriate pipe ends for stage `idx`, closes
 * every raw pipe fd (so EOF propagates correctly), then applies any file
 * redirections that should override the pipe connections, and finally calls
 * execvp().  Never returns.
 */
static void run_pipeline_stage(const Pipeline *pl, int idx,
                                int pipes[][2], int num_pipes) {
    /* connect stdin to the read end of the previous pipe */
    if (idx > 0)
        dup2(pipes[idx - 1][READ_END], STDIN_FILENO);

    /* connect stdout to the write end of the next pipe */
    if (idx < pl->num_commands - 1)
        dup2(pipes[idx][WRITE_END], STDOUT_FILENO);

    /* close every raw fd — leaving any write-end open would stall readers */
    for (int j = 0; j < num_pipes; j++) {
        close(pipes[j][READ_END]);
        close(pipes[j][WRITE_END]);
    }

    /* file redirections override the pipe connections established above */
    apply_redirections(&pl->cmds[idx]);

    execvp(pl->cmds[idx].args[0], pl->cmds[idx].args);
    fprintf(stderr, "Error: Command not found: %s\n", pl->cmds[idx].args[0]);
    exit(1);
}

/* ── execute_pipeline ────────────────────────────────────────────────────── */

void execute_pipeline(Pipeline *pl) {
    if (pl->num_commands == 1) {
        execute_single_command(&pl->cmds[0]);
        return;
    }

    int num_pipes = pl->num_commands - 1;
    int pipes[MAX_PIPE_CMDS][2];
    pid_t pids[MAX_PIPE_CMDS];

    /* create all pipes before forking so every child inherits them */
    for (int i = 0; i < num_pipes; i++) {
        if (pipe(pipes[i]) < 0) {
            perror("pipe");
            return;
        }
    }

    for (int i = 0; i < pl->num_commands; i++) {
        pids[i] = fork();
        if (pids[i] < 0) {
            perror("fork");
            /* close whatever pipes were opened before giving up */
            for (int j = 0; j < num_pipes; j++) {
                close(pipes[j][READ_END]);
                close(pipes[j][WRITE_END]);
            }
            return;
        }

        if (pids[i] == 0) {
            signal(SIGPIPE, SIG_DFL);
            run_pipeline_stage(pl, i, pipes, num_pipes);
            /* run_pipeline_stage never returns */
        }
    }

    /* parent closes its copies so the last child eventually gets EOF */
    for (int i = 0; i < num_pipes; i++) {
        close(pipes[i][READ_END]);
        close(pipes[i][WRITE_END]);
    }

    for (int i = 0; i < pl->num_commands; i++) {
        int status;
        waitpid(pids[i], &status, 0);
    }
}
