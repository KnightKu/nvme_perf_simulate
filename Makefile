TARGET := nvme_perf_model
SRCS := main.c cmd_sched.c cmd_generate.c cmd_pool.c bus_xfer.c \
        chan_cw_buf.c read_bus.c output_buffer.c host_port.c cw_path.c

CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

.PHONY: all clean test-legacy

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^

clean:
	$(RM) $(TARGET)

test-legacy: $(TARGET)
	bash scripts/run_legacy_io_tests.sh
