TARGET := nvme_perf_model
SRCS := main.c cmd_sched.c cmd_generate.c cmd_pool.c bus_xfer.c

CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^

clean:
	$(RM) $(TARGET)
