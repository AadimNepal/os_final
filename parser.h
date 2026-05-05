#ifndef PARSER_H
#define PARSER_H

#include "shell.h"

char *trim_whitespace(char *str);
void  init_command(Command *cmd);
int   parse_command(char *cmd_str, Command *cmd);

#endif /* PARSER_H */
