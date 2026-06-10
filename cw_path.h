#ifndef CW_PATH_H
#define CW_PATH_H

#include "output_buffer.h"

#include <stdint.h>

int cw_path_init(void);
void cw_path_cleanup(void);

int use_codeword_buffers(void);
int cw_cws_per_read_page(void);
int cw_cws_per_write_page(void);

int cw_try_schedule_read_data(int chan_id, uint64_t cur_time);
int cw_try_schedule_write_data(int chan_id, uint64_t cur_time);
int cw_chan_data_read_complete(int chan_id, uint64_t cur_time, int *inflight_cmds,
                               uint64_t *total_cmd, uint64_t *read_cmd);
int cw_chan_data_write_complete(int chan_id, uint64_t cur_time,
                                uint64_t *write_bytes);
int cw_total_read_cws(int act);
void cw_host_read_chunk_done(int act, const cw_run_ctx_t *ctx);
int cw_prepare_write_page(int act, int page_idx, uint64_t *write_bytes);
int cw_post_step(uint64_t sim_time, uint64_t *read_bytes, uint64_t *write_bytes,
                 const cw_run_ctx_t *ctx);
uint64_t cw_next_event(uint64_t sim_time);

#endif
