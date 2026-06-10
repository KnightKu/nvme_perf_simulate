#ifndef CHAN_CW_BUF_H
#define CHAN_CW_BUF_H

#include <stdint.h>

typedef enum {
    CW_SLOT_EMPTY,
    CW_SLOT_FILLING,
    CW_SLOT_READY,
    CW_SLOT_XFER,
} cw_slot_state_t;

typedef struct cw_slot_s {
    cw_slot_state_t state;
    int act;
    int page_idx;
    int cw_idx;
} cw_slot_t;

typedef struct chan_cw_buf_s {
    cw_slot_t read[2];
    cw_slot_t prog[2];
} chan_cw_buf_t;

int chan_cw_buf_init(int chan_num);
void chan_cw_buf_cleanup(void);

int chan_cw_read_acquire(int chan_id);
void chan_cw_read_mark_ready(int chan_id, int slot, int act, int page_idx,
                             int cw_idx);
int chan_cw_read_pick_ready(int chan_id);
void chan_cw_read_release(int chan_id, int slot);

int chan_cw_prog_acquire(int chan_id);
void chan_cw_prog_mark_ready(int chan_id, int slot, int act, int page_idx,
                             int cw_idx);
int chan_cw_prog_pick_ready(int chan_id, int act, int page_idx, int cw_idx);
void chan_cw_prog_release(int chan_id, int slot);

const cw_slot_t *chan_cw_read_slot_info(int chan_id, int slot);

#endif
