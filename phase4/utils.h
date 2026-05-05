#ifndef UTILS_H
#define UTILS_H

// a Redirs hold file redirections for a single stage
typedef struct {
    char *in_file;   // for <
    char *out_file;  // for >
    char *err_file;  // for 2>
} Redirs;

// a Stage is a command with its args and redirections 
typedef struct {
    char **argv;  // compacted argv (contains no redirections or file names) for execvp
    Redirs r;     // redirections for this stage
} Stage;

char** parse_command(const char* input);  // function to parse a command string into an array of arguments
int parse_redirs(char **args, Redirs *R, char **errmsg);  // scan args to extract redirections and compact argv

// build pipeline stages from a flat token list (args with NULL terminator)
// returns number of stages on success (>=1), or -1 on error and sets *errmsg
int build_pipeline(char **tokens, Stage **stages_out, int *nstages_out, const char **errmsg);

// execute an already-built pipeline
// returns 0 on success; -1 on immediate setup failure
// NEW argument: err_fd is the fd of the pipe for communicating error msgs between parent and child
int exec_pipeline(Stage *stages, int nstages, int err_fd);

// writes a formatted error message directly to a file descriptor (e.g., a pipe or stderr).
// works like printf(), but instead of printing to stdout, it sends the formatted output
// to the provided file descriptor. Useful for mirroring error messages to another process
void err_write(int err_fd, const char *fmt, ...);

#endif //UTILS_H