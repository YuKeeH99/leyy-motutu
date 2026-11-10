#ifndef LINGXIN_TRACE_H
#define LINGXIN_TRACE_H

int lingxin_trace_set(const char *step);
int lingxin_trace_clear(void);
int lingxin_trace_get(char **out);
int lingxin_trace_try_get(char **out);

#endif
