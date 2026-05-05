/*
 * parser.c - input tokenisation for myshell
 *
 * Provides two public functions:
 *   trim_whitespace()  - strips leading/trailing whitespace from a string
 *   parse_command()    - converts a single command string into a Command struct
 *
 * Design note: the parser walks the string character-by-character rather than
 * relying solely on strtok() so that the two-character operator "2>" can be
 * identified before the single-character ">" is tested.  Using strtok() on
 * whitespace would misread "2>" by storing "2" as a plain argument and then
 * treating ">" as output redirection.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "parser.h"

/* ── init_command ────────────────────────────────────────────────────────── */

void init_command(Command *cmd) {
    cmd->arg_count   = 0;
    cmd->input_file  = NULL;
    cmd->output_file = NULL;
    cmd->error_file  = NULL;
    cmd->append_mode = 0;
    for (int i = 0; i < MAX_ARGS; i++)
        cmd->args[i] = NULL;
}

/* ── trim_whitespace ─────────────────────────────────────────────────────── */

/*
 * trim_whitespace - remove leading and trailing whitespace from str.
 *
 * The function advances the pointer past any leading spaces/tabs/newlines,
 * then walks backward from the end to null-terminate after the last
 * non-whitespace character.  No heap allocation is performed; the returned
 * pointer points into the original buffer.
 */
char *trim_whitespace(char *str) {
    /* skip all leading whitespace characters */
    while (*str == ' ' || *str == '\t' || *str == '\n') {
        str++;
    }

    /* if the string is entirely whitespace, return the terminator */
    if (*str == '\0') {
        return str;
    }

    /* find the last character and walk backward past trailing whitespace */
    char *end = str + strlen(str) - 1;
    while (end > str && (*end == ' ' || *end == '\t' || *end == '\n')) {
        end--;
    }

    /* place the null terminator immediately after the last real character */
    *(end + 1) = '\0';
    return str;
}

/* ── parse_command ───────────────────────────────────────────────────────── */

/*
 * parse_command - tokenise cmd_str and populate *cmd.
 *
 * The function scans left-to-right.  At each non-space position it checks for
 * redirection operators in priority order (2> before > to avoid ambiguity),
 * then falls through to treating the token as a plain argument.
 *
 * For each redirection operator the function reads the very next
 * whitespace-delimited token as the filename; if no token follows, an error
 * is printed and -1 is returned so the caller can abort the command.
 *
 * On success the args[] array is null-terminated and 0 is returned.
 */
int parse_command(char *cmd_str, Command *cmd) {
    init_command(cmd);

    /* trim once so the length calculation below is accurate */
    cmd_str = trim_whitespace(cmd_str);

    if (strlen(cmd_str) == 0) {
        return -1;
    }

    int i   = 0;
    int len = (int)strlen(cmd_str);

    while (i < len) {
        /* skip inter-token spaces */
        while (i < len && cmd_str[i] == ' ') {
            i++;
        }
        if (i >= len) {
            break;
        }

        /* ── 2> : stderr redirection ─────────────────────────────────────── */
        if (cmd_str[i] == '2' && (i + 1) < len && cmd_str[i + 1] == '>') {
            i += 2; /* consume both characters of the operator */

            /* skip spaces between the operator and the filename */
            while (i < len && cmd_str[i] == ' ') {
                i++;
            }

            /* no filename following the operator is a parse error */
            if (i >= len) {
                fprintf(stderr, "Error: Error output file not specified.\n");
                return -1;
            }

            /* read the filename token (delimited by a space or end-of-string) */
            int start = i;
            while (i < len && cmd_str[i] != ' ') {
                i++;
            }
            if (i < len) {
                cmd_str[i] = '\0'; /* null-terminate the filename in-place */
                i++;
            }
            cmd->error_file = &cmd_str[start];
        }

        /* ── > : stdout redirection ──────────────────────────────────────── */
        else if (cmd_str[i] == '>') {
            i++; /* consume '>' */
             /* check for >> (append redirection) */
            if (i < len && cmd_str[i] == '>') {
                i++; /* consume second '>' */
                cmd->append_mode = 1;
            }

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
            if (i < len) {
                cmd_str[i] = '\0';
                i++;
            }
            cmd->output_file = &cmd_str[start];
        }

        /* ── < : stdin redirection ───────────────────────────────────────── */
        else if (cmd_str[i] == '<') {
            i++; /* consume '<' */

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
            if (i < len) {
                cmd_str[i] = '\0';
                i++;
            }
            cmd->input_file = &cmd_str[start];
        }

        /* ── regular argument token ──────────────────────────────────────── */
        else {
            /*
             * A token may consist of adjacent quoted and unquoted parts
             * with no whitespace between them, e.g. "join"ed → joined, or
             * pre'mid'post → premidpost.  We accumulate all adjacent chunks
             * into token_buf, then write the assembled result back into
             * cmd_str at 'start' so the arg pointer remains valid.
             *
             * The outer loop keeps consuming chunks until it reaches a token
             * boundary: space, <, >, 2>, or end of string.  Inside the loop,
             * each iteration handles either one quoted chunk or one unquoted
             * chunk before checking the boundary condition again.
             */
            char token_buf[MAX_INPUT_LENGTH];
            int  token_len = 0;
            int  start = i;   /* write assembled token here in cmd_str */

            while (i < len
                   && cmd_str[i] != ' '
                   && cmd_str[i] != '<'
                   && cmd_str[i] != '>') {

                /* stop at 2> before the plain > check gets a chance to fire */
                if (cmd_str[i] == '2' && (i + 1) < len && cmd_str[i + 1] == '>') {
                    break;
                }

                if (cmd_str[i] == '"' || cmd_str[i] == '\'') {
                    /* ── quoted chunk: copy content between the delimiters ── */
                    char quote_char = cmd_str[i];
                    i++;   /* skip opening quote */

                    while (i < len && cmd_str[i] != quote_char) {
                        if (token_len < MAX_INPUT_LENGTH - 1) {
                            token_buf[token_len++] = cmd_str[i];
                        }
                        i++;
                    }
                    if (i < len) {
                        i++;   /* skip closing quote */
                    }
                } else {
                    /*
                     * ── unquoted chunk: copy characters until we hit a
                     * boundary or the start of a quoted section ───────────
                     * We stop at '"'/"'" so the outer loop can re-enter the
                     * quoted branch for the next adjacent chunk.
                     */
                    while (i < len
                           && cmd_str[i] != ' '
                           && cmd_str[i] != '"'
                           && cmd_str[i] != '\''
                           && cmd_str[i] != '<'
                           && cmd_str[i] != '>') {
                        if (cmd_str[i] == '2' && (i + 1) < len
                            && cmd_str[i + 1] == '>') {
                            break;
                        }
                        if (token_len < MAX_INPUT_LENGTH - 1) {
                            token_buf[token_len++] = cmd_str[i];
                        }
                        i++;
                    }
                }
            }

            /* skip the single space that terminates this token */
            if (i < len && cmd_str[i] == ' ') {
                i++;
            }

            /*
             * Write the assembled token back into cmd_str starting at
             * 'start', then null-terminate it.  Writing back is safe
             * because token_len <= (characters consumed from cmd_str),
             * so we never write past what we originally read.
             * The arg pointer is set into cmd_str so it remains valid
             * for the lifetime of the Command struct.
             */
            token_buf[token_len] = '\0';
            strncpy(&cmd_str[start], token_buf, (size_t)(token_len + 1));
            cmd->args[cmd->arg_count] = &cmd_str[start];
            cmd->arg_count++;
        }
    }

    /* execvp() requires the argument array to end with a NULL pointer */
    cmd->args[cmd->arg_count] = NULL;
    return 0;
}
