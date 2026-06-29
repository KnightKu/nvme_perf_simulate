#ifndef CONFIG_H
#define CONFIG_H

#include "cmd_sched.h"

const char *perf_default_config_path(void);
void perf_config_defaults(perf_config_t *cfg);
int perf_load_config(const char *path, perf_config_t *cfg);

#endif
