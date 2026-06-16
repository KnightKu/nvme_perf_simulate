#ifndef WRITE_CACHE_H
#define WRITE_CACHE_H

#include <stdint.h>

typedef struct write_cache_ctx_s {
    int *inflight_cmds;
    uint64_t *total_cmd;
    uint64_t *write_cmd;
    uint64_t *write_bytes;
    uint64_t *nand_program_pages;
} write_cache_ctx_t;

void write_cache_init(void);
void write_cache_cleanup(void);
int write_cache_on_write_cmd(int act, const write_cache_ctx_t *ctx);
int write_cache_try_schedule_flush(int chan_id, uint64_t cur_time);
int write_cache_chan_data_complete(int chan_id, uint64_t cur_time);
int write_cache_tprog_complete(int chan_id, int die, int slot, uint64_t cur_time,
                               uint64_t *nand_program_pages);

#endif
