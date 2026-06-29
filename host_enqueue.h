#ifndef HOST_ENQUEUE_H
#define HOST_ENQUEUE_H

void enqueue_cmd(int chan_id, int die_in_chan, int op, int act);
void enqueue_host_cmd(int act, int op);

#endif
