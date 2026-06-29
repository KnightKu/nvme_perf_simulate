#ifndef STRIPE_H
#define STRIPE_H

void select_target(int *chan_id, int *die_in_chan);
void enqueue_cmd(int chan_id, int die_in_chan, int op, int act);
void enqueue_host_cmd(int act, int op);

#endif
