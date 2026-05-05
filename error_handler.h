#ifndef ERROR_HANDLER_H
#define ERROR_HANDLER_H

#include "shell.h"

void report_syserr(const char *message);
void report_error(const char *message);
int  pipeline_valid(const Pipeline *pipeline);

#endif /* ERROR_HANDLER_H */
