// myshell.c - custom shell supporting commands, args, redirection (<, >, 2>), pipes, error handling

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>

#define MAX_INPUT_LENGTH 1024   // max characters the user can type
#define MAX_ARGS         64     // max arguments per command
#define MAX_PIPE_CMDS    16     // max number of commands in a pipeline

// holds all info about a single command: its args and any redirection files
typedef struct {
    char *args[MAX_ARGS];   // argument list, null-terminated for execvp
    int arg_count;          // number of arguments
    char *input_file;       // filename for < redirection
    char *output_file;      // filename for > redirection
    char *error_file;       // filename for 2> redirection
} Command;

// strips leading and trailing whitespace, returns pointer to trimmed portion
char *trim_whitespace(char *str) {
    // skip leading spaces/tabs/newlines
    while (*str == ' ' || *str == '\t' || *str == '\n') {
        str++;
    }

    if (*str == '\0') {
        return str;
    }

    // find end and walk backward past trailing whitespace
    char *end = str + strlen(str) - 1;
    while (end > str && (*end == ' ' || *end == '\t' || *end == '\n')) {
        end--;
    }

    // null-terminate after last non-whitespace char
    *(end + 1) = '\0';
    return str;
}

// parses a single command string into a Command struct
// detects <, >, 2> and separates args from redirection targets
// returns 0 on success, -1 on error
int parse_command(char *cmd_str, Command *cmd) {
    // initialize everything to clean state
    cmd->arg_count = 0;
    cmd->input_file = NULL;
    cmd->output_file = NULL;
    cmd->error_file = NULL;

    cmd_str = trim_whitespace(cmd_str);

    if (strlen(cmd_str) == 0) {
        return -1;
    }

    int i = 0;
    int len = strlen(cmd_str);

    // walk through string char by char looking for redirections and args
    while (i < len) {
        // skip spaces between tokens
        while (i < len && cmd_str[i] == ' ') {
            i++;
        }

        if (i >= len) {
            break;
        }

        // check for 2> first so '2' doesn't get eaten as a regular arg
        if (cmd_str[i] == '2' && (i + 1) < len && cmd_str[i + 1] == '>') {
            i += 2;

            // skip spaces after 2>
            while (i < len && cmd_str[i] == ' ') {
                i++;
            }

            if (i >= len) {
                fprintf(stderr, "Error: Error output file not specified.\n");
                return -1;
            }

            // grab the filename
            int start = i;
            while (i < len && cmd_str[i] != ' ') {
                i++;
            }

            cmd_str[i] = '\0';
            cmd->error_file = &cmd_str[start];

            if (i < len) {
                i++;
            }
        }
        // check for > (output redirection)
        else if (cmd_str[i] == '>') {
            i++;

            while (i < len && cmd_str[i] == ' ') {
                i++;
            }

            if (i >= len) {
                fprintf(stderr, "Error: Output file not specified.\n");
                return -1;
            }

            int start = i;
            while (i < len && cmd_str[i] != ' ') {
                i++;
            }

            cmd_str[i] = '\0';
            cmd->output_file = &cmd_str[start];

            if (i < len) {
                i++;
            }
        }
        // check for < (input redirection)
        else if (cmd_str[i] == '<') {
            i++;

            while (i < len && cmd_str[i] == ' ') {
                i++;
            }

            if (i >= len) {
                fprintf(stderr, "Error: Input file not specified.\n");
                return -1;
            }

            int start = i;
            while (i < len && cmd_str[i] != ' ') {
                i++;
            }

            cmd_str[i] = '\0';
            cmd->input_file = &cmd_str[start];

            if (i < len) {
                i++;
            }
        }
        // regular argument (command name, flag, etc.)
        else {
            int start = i;

            // read until space or redirection char
            while (i < len && cmd_str[i] != ' '
                   && cmd_str[i] != '<' && cmd_str[i] != '>') {
                // stop if we hit 2> so we don't eat the '2'
                if (cmd_str[i] == '2' && (i + 1) < len && cmd_str[i + 1] == '>') {
                    break;
                }
                i++;
            }

            // null-terminate if we stopped at a space
            if (i < len && cmd_str[i] == ' ') {
                cmd_str[i] = '\0';
                i++;
            }

            cmd->args[cmd->arg_count] = &cmd_str[start];
            cmd->arg_count++;
        }
    }

    // execvp needs null-terminated args array
    cmd->args[cmd->arg_count] = NULL;
    return 0;
}

// runs a single command (no pipes) in a child process, handles <, >, 2>
void execute_single_command(Command *cmd) {
    pid_t pid = fork();

    if (pid < 0) {
        perror("fork");
        return;
    }

    if (pid == 0) {
        // child process: set up redirections then exec

        // input redirection: open file and point stdin to it
        if (cmd->input_file != NULL) {
            int fd_in = open(cmd->input_file, O_RDONLY);
            if (fd_in < 0) {
                fprintf(stderr, "Error: File not found: %s\n", cmd->input_file);
                exit(1);
            }
            dup2(fd_in, STDIN_FILENO);
            close(fd_in);
        }

        // output redirection: open/create file and point stdout to it
        if (cmd->output_file != NULL) {
            int fd_out = open(cmd->output_file,
                              O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd_out < 0) {
                perror("open output file");
                exit(1);
            }
            dup2(fd_out, STDOUT_FILENO);
            close(fd_out);
        }

        // error redirection: open/create file and point stderr to it
        if (cmd->error_file != NULL) {
            int fd_err = open(cmd->error_file,
                              O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd_err < 0) {
                perror("open error file");
                exit(1);
            }
            dup2(fd_err, STDERR_FILENO);
            close(fd_err);
        }

        // replace child with the actual command; if execvp returns it failed
        execvp(cmd->args[0], cmd->args);
        fprintf(stderr, "Error: Command not found: %s\n", cmd->args[0]);
        exit(1);

    } else {
        // parent: wait for child to finish
        int status;
        waitpid(pid, &status, 0);
    }
}

// runs n commands connected by pipes (n-1 pipes needed)
void execute_pipeline(Command cmds[], int num_cmds) {
    int pipe_fds[MAX_PIPE_CMDS][2]; // pipe_fds[i][0]=read end, [1]=write end

    // create all pipes upfront
    int i;
    for (i = 0; i < num_cmds - 1; i++) {
        if (pipe(pipe_fds[i]) < 0) {
            perror("pipe");
            return;
        }
    }

    pid_t pids[MAX_PIPE_CMDS]; // store child pids so parent can wait for all

    for (i = 0; i < num_cmds; i++) {
        pids[i] = fork();

        if (pids[i] < 0) {
            perror("fork");
            return;
        }

        if (pids[i] == 0) {
            // child for command i

            // not the first cmd: read stdin from previous pipe's read end
            if (i > 0) {
                dup2(pipe_fds[i - 1][0], STDIN_FILENO);
            }

            // not the last cmd: write stdout to current pipe's write end
            if (i < num_cmds - 1) {
                dup2(pipe_fds[i][1], STDOUT_FILENO);
            }

            // close all pipe fds in child; not closing causes hangs (no EOF)
            int j;
            for (j = 0; j < num_cmds - 1; j++) {
                close(pipe_fds[j][0]);
                close(pipe_fds[j][1]);
            }

            // handle file redirections on top of pipe setup
            if (cmds[i].input_file != NULL) {
                int fd_in = open(cmds[i].input_file, O_RDONLY);
                if (fd_in < 0) {
                    fprintf(stderr, "Error: File not found: %s\n",
                            cmds[i].input_file);
                    exit(1);
                }
                dup2(fd_in, STDIN_FILENO);
                close(fd_in);
            }

            if (cmds[i].output_file != NULL) {
                int fd_out = open(cmds[i].output_file,
                                  O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (fd_out < 0) {
                    perror("open output file");
                    exit(1);
                }
                dup2(fd_out, STDOUT_FILENO);
                close(fd_out);
            }

            if (cmds[i].error_file != NULL) {
                int fd_err = open(cmds[i].error_file,
                                  O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (fd_err < 0) {
                    perror("open error file");
                    exit(1);
                }
                dup2(fd_err, STDERR_FILENO);
                close(fd_err);
            }

            // run the command
            execvp(cmds[i].args[0], cmds[i].args);
            fprintf(stderr, "Error: Command not found in pipe sequence: %s\n",
                    cmds[i].args[0]);
            exit(1);
        }
    }

    // parent: close all pipe fds so children can get EOF
    for (i = 0; i < num_cmds - 1; i++) {
        close(pipe_fds[i][0]);
        close(pipe_fds[i][1]);
    }

    // wait for every child
    for (i = 0; i < num_cmds; i++) {
        int status;
        waitpid(pids[i], &status, 0);
    }
}

// splits full input by pipes, validates, parses each segment, and runs
void process_input(char *input) {
    char input_copy[MAX_INPUT_LENGTH];
    strcpy(input_copy, input);

    // check for pipe at the very beginning
    char *trimmed = trim_whitespace(input_copy);
    if (trimmed[0] == '|') {
        fprintf(stderr, "Error: Command missing before pipe.\n");
        return;
    }

    // check for pipe at the very end
    int input_len = strlen(trimmed);
    if (input_len > 0 && trimmed[input_len - 1] == '|') {
        fprintf(stderr, "Error: Command missing after pipe.\n");
        return;
    }

    // split by | into segments
    char *pipe_segments[MAX_PIPE_CMDS];
    int num_segments = 0;

    char input_for_split[MAX_INPUT_LENGTH];
    strcpy(input_for_split, input);

    char *segment = strtok(input_for_split, "|");
    while (segment != NULL && num_segments < MAX_PIPE_CMDS) {
        pipe_segments[num_segments] = segment;
        num_segments++;
        segment = strtok(NULL, "|");
    }

    // make sure no segment is empty (catches "cmd1 | | cmd2")
    int i;
    for (i = 0; i < num_segments; i++) {
        char *trimmed_seg = trim_whitespace(pipe_segments[i]);
        if (strlen(trimmed_seg) == 0) {
            fprintf(stderr, "Error: Empty command between pipes.\n");
            return;
        }
    }

    // parse each segment into a Command struct
    Command cmds[MAX_PIPE_CMDS];

    for (i = 0; i < num_segments; i++) {
        if (parse_command(pipe_segments[i], &cmds[i]) != 0) {
            return; // parse_command already printed the error
        }

        if (cmds[i].arg_count == 0) {
            fprintf(stderr, "Error: Empty command.\n");
            return;
        }
    }

    // single command vs pipeline
    if (num_segments == 1) {
        execute_single_command(&cmds[0]);
    } else {
        execute_pipeline(cmds, num_segments);
    }
}

// main loop: print prompt, read input, dispatch commands, repeat until "exit"
int main(void) {
    char input[MAX_INPUT_LENGTH];

    while (1) {
        printf("$ ");
        fflush(stdout);

        // fgets returns NULL on EOF (ctrl+d)
        if (fgets(input, MAX_INPUT_LENGTH, stdin) == NULL) {
            printf("\n");
            break;
        }

        // strip trailing newline from fgets
        int len = strlen(input);
        if (len > 0 && input[len - 1] == '\n') {
            input[len - 1] = '\0';
        }

        // skip blank lines
        char *trimmed_input = trim_whitespace(input);
        if (strlen(trimmed_input) == 0) {
            continue;
        }

        // exit command
        if (strcmp(trimmed_input, "exit") == 0) {
            break;
        }

        process_input(trimmed_input);
    }

    return 0;
}