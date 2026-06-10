#ifndef CMD_GENERATE_H
#define CMD_GENERATE_H

#include <stdint.h>

int cmd_generate_try(int tmp_cmd_cnt, int *inflight_cmds, uint64_t *pool_rejects);

#endif
