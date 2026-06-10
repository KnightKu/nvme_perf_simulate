#ifndef BUS_XFER_H
#define BUS_XFER_H

#include <stdint.h>

void bus_xfer_init(void);
void bus_xfer_cleanup(void);
int bus_process(uint64_t sim_time, uint64_t *bus_xfers, uint64_t *bus_bytes);
uint64_t bus_xfer_next_event(uint64_t sim_time);

#endif
