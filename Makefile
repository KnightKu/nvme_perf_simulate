TARGET := randread_perf
SRCS := randread_perf.c

CXX ?= g++
CXXFLAGS ?= -O2 -std=c++11 -Wall -Wextra

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CXX) $(CXXFLAGS) -o $@ $^

clean:
	$(RM) $(TARGET)
