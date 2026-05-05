#ifndef EXECUTOR_H
#define EXECUTOR_H

#include <sys/types.h>
#include "shell.h"

#define READ_END  0
#define WRITE_END 1

void execute_single_command(Command *cmd);
void execute_pipeline(Pipeline *pl);

#endif /* EXECUTOR_H */
