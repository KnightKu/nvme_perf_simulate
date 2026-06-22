TARGET := nvme_perf_model
SRCS := main.c cmd_sched.c cmd_generate.c

CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

.PHONY: all clean test-nand summarize-nand

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^

clean:
	$(RM) $(TARGET)

test-nand: $(TARGET)
	bash scripts/run_nand_tests.sh

summarize-nand:
	python3 scripts/summarize_nand.py tests/out_nand
