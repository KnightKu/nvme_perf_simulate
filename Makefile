TARGET := nvme_perf_model
SRCS := main.c cmd_sched.c

CXX ?= g++
CXXFLAGS ?= -O2 -std=c++11 -Wall -Wextra

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CXX) $(CXXFLAGS) -o $@ $^

clean:
	$(RM) $(TARGET)
