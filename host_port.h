#ifndef HOST_PORT_H
#define HOST_PORT_H

#include <stdint.h>

int host_port_init(int qd);
void host_port_cleanup(void);
int host_port_try_feed_write(int chan_id, int act, int page_idx, int cw_idx,
                             uint64_t *write_bytes);
int host_port_process(uint64_t sim_time, uint64_t *write_bytes);

#endif
