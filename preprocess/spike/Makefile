# preprocess/spike -- Agent H (trace generation)
CXX      ?= g++
CXXFLAGS ?= -O2 -g -std=c++17 -Wall -Wextra -Wno-unused-parameter
INC      := -I../../include -I.

BINS := bstf_gen bstf_check

all: $(BINS)

bstf_gen: bstf_gen.cc rv64_decode.h ../../include/bstf.h
	$(CXX) $(CXXFLAGS) $(INC) -o $@ bstf_gen.cc

bstf_check: bstf_check.cc ../../include/bstf.h
	$(CXX) $(CXXFLAGS) $(INC) -o $@ bstf_check.cc

# ---- minimal self test: 200k instructions straight out of spike ----
test: $(BINS)
	./run_coremark.sh --self-test

clean:
	rm -f $(BINS) *.o
	rm -rf selftest elf

.PHONY: all test clean
