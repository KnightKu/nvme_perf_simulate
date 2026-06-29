#ifndef TIMING_H
#define TIMING_H

#include "cmd_sched.h"
#include "sched_internal.h"

int perf_config_validate(const perf_config_t *cfg);
int perf_derived_init(const perf_config_t *cfg, perf_derived_t *d);

#endif
