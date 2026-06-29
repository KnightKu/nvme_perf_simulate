TARGET := nvme_perf_model
SRCS := main.c config.c timing.c stripe.c nand_sched.c stats.c cmd_generate.c

CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

.PHONY: all clean test-nand nand-core summarize-nand

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^

clean:
	$(RM) $(TARGET)

nand-core: test-nand

test-nand: $(TARGET)
	bash scripts/run_nand_tests.sh

summarize-nand:
	python3 scripts/summarize_nand.py tests/out_nand
