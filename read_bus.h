#ifndef READ_BUS_H
#define READ_BUS_H

#include <stdint.h>

void read_bus_init(void);
void read_bus_cleanup(void);
int read_bus_process(uint64_t sim_time, uint64_t *read_bus_bytes);
uint64_t read_bus_next_event(uint64_t sim_time);

#endif
