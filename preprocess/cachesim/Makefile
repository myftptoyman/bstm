# BSTM offline cache simulator (Agent B)
CXX      ?= g++
BSTF_INC ?= ../../include
CXXFLAGS := -O2 -std=c++17 -Wall -Wextra -I$(BSTF_INC) $(EXTRA_CXXFLAGS)
BIN      := cachesim

all: $(BIN)

$(BIN): cachesim.cpp $(BSTF_INC)/bstf.h
	$(CXX) $(CXXFLAGS) -o $@ cachesim.cpp

test: $(BIN)
	./test/run_tests.sh

clean:
	rm -f $(BIN)
	rm -rf build

.PHONY: all test clean
