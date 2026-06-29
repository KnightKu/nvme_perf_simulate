#include "stripe.h"

#include "sched_internal.h"

#include <stdlib.h>

void select_target(int *chan_id, int *die_in_chan) {
    if (g_state.cfg.stripe_mode == STRIPE_CHANNEL_MAJOR) {
        int ch = g_state.rr_chan % g_state.cfg.chan_num;
        int die = g_state.stripe_cursor[ch] % g_state.d.die_per_chan;

        g_state.rr_chan = (g_state.rr_chan + 1) % g_state.cfg.chan_num;
        g_state.stripe_cursor[ch] =
            (g_state.stripe_cursor[ch] + 1) % g_state.d.die_per_chan;
        *chan_id = ch;
        *die_in_chan = die;
        return;
    }

    if (g_state.cfg.io_pattern == IO_PATTERN_SEQUENTIAL) {
        int global_die = g_state.next_die;

        g_state.next_die = (g_state.next_die + 1) % g_state.cfg.die_num;
        *chan_id = global_die % g_state.cfg.chan_num;
        *die_in_chan = global_die / g_state.cfg.chan_num;
        return;
    }

    {
        int global_die = rand() % g_state.cfg.die_num;

        *chan_id = global_die % g_state.cfg.chan_num;
        *die_in_chan = global_die / g_state.cfg.chan_num;
    }
}

void enqueue_cmd(int chan_id, int die_in_chan, int op, int act) {
    die_ctx_t *ctx = die_ctx_at(chan_id, die_in_chan);
    queue_push(&ctx->q[op], act);
}

static int page_stripe_device_pages(void) {
    return g_state.cfg.chan_num * g_state.d.die_per_chan;
}

static int page_stripe_block_slots(void) {
    int device_pages = page_stripe_device_pages();
    int ppb = g_state.d.pages_per_block;

    if (ppb <= 0 || device_pages <= 0) {
        return 1;
    }
    return device_pages / ppb;
}

static void assign_page_stripe_base(int act) {
    int ppb = g_state.d.pages_per_block;
    int device_pages = page_stripe_device_pages();
    int slots = page_stripe_block_slots();

    if (g_state.cfg.io_pattern == IO_PATTERN_SEQUENTIAL) {
        g_state.cmd_stripe_base[act] = g_state.global_page_stripe;
        g_state.global_page_stripe += ppb;
        if (device_pages > 0) {
            g_state.global_page_stripe %= device_pages;
        }
        return;
    }

    if (slots <= 0) {
        slots = 1;
    }
    g_state.cmd_stripe_base[act] = (rand() % slots) * ppb;
}

void enqueue_host_cmd(int act, int op) {
    g_state.cmd_page_stripe[act] = 0;

    if (g_state.cfg.stripe_mode == STRIPE_PAGE &&
        g_state.d.pages_per_block > 1 && op != OP_ERASE) {
        int chan_id;
        int die_in_chan;

        g_state.cmd_page_stripe[act] = 1;
        assign_page_stripe_base(act);
        page_stripe_target(g_state.cmd_stripe_base[act], 0, &chan_id,
                           &die_in_chan);
        g_state.cmd_target_chan[act] = chan_id;
        g_state.cmd_target_die[act] = die_in_chan;
        enqueue_cmd(chan_id, die_in_chan, op, act);
        return;
    }

    select_target(&g_state.cmd_target_chan[act],
                  &g_state.cmd_target_die[act]);
    enqueue_cmd(g_state.cmd_target_chan[act], g_state.cmd_target_die[act], op,
                act);
}
