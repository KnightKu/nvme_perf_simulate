#include "chan_cw_buf.h"

#include "sched_internal.h"

#include <stdlib.h>
#include <string.h>

static chan_cw_buf_t *s_chan_cw;

static int find_empty_slot(cw_slot_t *slots, int count) {
    int i;

    for (i = 0; i < count; i++) {
        if (slots[i].state == CW_SLOT_EMPTY) {
            return i;
        }
    }
    return -1;
}

int chan_cw_buf_init(int chan_num) {
    s_chan_cw = (chan_cw_buf_t *)calloc(chan_num, sizeof(chan_cw_buf_t));
    if (!s_chan_cw) {
        return -1;
    }
    return 0;
}

void chan_cw_buf_cleanup(void) {
    free(s_chan_cw);
    s_chan_cw = NULL;
}

static chan_cw_buf_t *chan_cw_at(int chan_id) {
    return &s_chan_cw[chan_id];
}

int chan_cw_read_acquire(int chan_id) {
    return find_empty_slot(chan_cw_at(chan_id)->read, 2);
}

void chan_cw_read_mark_ready(int chan_id, int slot, int act, int page_idx,
                             int cw_idx) {
    cw_slot_t *ps = &chan_cw_at(chan_id)->read[slot];

    ps->state = CW_SLOT_READY;
    ps->act = act;
    ps->page_idx = page_idx;
    ps->cw_idx = cw_idx;
}

int chan_cw_read_pick_ready(int chan_id) {
    int i;

    for (i = 0; i < 2; i++) {
        if (chan_cw_at(chan_id)->read[i].state == CW_SLOT_READY) {
            return i;
        }
    }
    return -1;
}

void chan_cw_read_release(int chan_id, int slot) {
    cw_slot_t *ps = &chan_cw_at(chan_id)->read[slot];

    ps->state = CW_SLOT_EMPTY;
    ps->act = 0xFFF;
    ps->page_idx = -1;
    ps->cw_idx = -1;
}

int chan_cw_prog_acquire(int chan_id) {
    return find_empty_slot(chan_cw_at(chan_id)->prog, 2);
}

void chan_cw_prog_mark_ready(int chan_id, int slot, int act, int page_idx,
                             int cw_idx) {
    cw_slot_t *ps = &chan_cw_at(chan_id)->prog[slot];

    ps->state = CW_SLOT_READY;
    ps->act = act;
    ps->page_idx = page_idx;
    ps->cw_idx = cw_idx;
}

int chan_cw_prog_pick_ready(int chan_id, int act, int page_idx, int cw_idx) {
    int i;

    for (i = 0; i < 2; i++) {
        cw_slot_t *ps = &chan_cw_at(chan_id)->prog[i];

        if (ps->state == CW_SLOT_READY && ps->act == act &&
            ps->page_idx == page_idx && ps->cw_idx == cw_idx) {
            return i;
        }
    }
    return -1;
}

void chan_cw_prog_release(int chan_id, int slot) {
    cw_slot_t *ps = &chan_cw_at(chan_id)->prog[slot];

    ps->state = CW_SLOT_EMPTY;
    ps->act = 0xFFF;
    ps->page_idx = -1;
    ps->cw_idx = -1;
}

const cw_slot_t *chan_cw_read_slot_info(int chan_id, int slot) {
    if (slot < 0 || slot > 1) {
        return NULL;
    }
    return &chan_cw_at(chan_id)->read[slot];
}
