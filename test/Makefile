# BSTM test / bench / equiv  (Agent G)
TOP      := $(abspath ..)
RT       := $(TOP)/lib/runtime
CC       ?= gcc
OPT      ?= -O3
ARCH     ?= -march=x86-64-v3
CFLAGS   ?= $(OPT) $(ARCH) -std=c11 -Wall -Wextra -I$(RT) -I$(TOP)/include -Imock -Iunit -pthread
LDLIBS    = -pthread
LIB       = $(RT)/libbstm.a

UNIT  := t_transpose t_bstf t_refill t_refill_wp t_batch t_stats t_pool t_end2end
BENCH := bench_transpose bench_refill
TRACES := traces/synth0.bstf traces/synth1.bstf

all: $(addprefix build/,$(UNIT) $(BENCH)) build/mkbstf build/bstfdump

$(LIB):
	$(MAKE) -C $(RT)

build:
	@mkdir -p build

build/mkbstf: tools/mkbstf.c $(TOP)/include/bstf.h | build
	$(CC) $(CFLAGS) $< -o $@

build/bstfdump: tools/bstfdump.c $(LIB) | build
	$(CC) $(CFLAGS) $< $(LIB) -o $@ $(LDLIBS)

build/mock_ooo_bs.o: mock/mock_ooo_bs.c | build
	$(CC) $(CFLAGS) -c $< -o $@

build/t_%: unit/t_%.c $(LIB) build/mock_ooo_bs.o | build
	$(CC) $(CFLAGS) $< build/mock_ooo_bs.o $(LIB) -o $@ $(LDLIBS)

build/bench_%: bench/bench_%.c $(LIB) build/mock_ooo_bs.o | build
	$(CC) $(CFLAGS) $< build/mock_ooo_bs.o $(LIB) -o $@ $(LDLIBS)

traces/synth0.bstf: build/mkbstf
	@mkdir -p traces
	./build/mkbstf -o traces/synth0 -n 20000 -d 32 -s 1 --simpoint 0
traces/synth1.bstf: build/mkbstf
	@mkdir -p traces
	./build/mkbstf -o traces/synth1 -n 20000 -d 32 -s 7 --simpoint 1

test: all $(TRACES)
	@rc=0; for t in $(UNIT); do ./build/$$t || rc=1; done; \
	 echo "----"; [ $$rc = 0 ] && echo "ALL UNIT TESTS PASS" || echo "UNIT TESTS FAILED"; exit $$rc

bench: all $(TRACES)
	@./build/bench_transpose; echo; ./build/bench_refill

equiv:
	$(MAKE) -C equiv run

# 用 Agent H 產生的真 trace 做整合檢查（overlay 還沒好，所以用 BSTF_LAX）
REAL ?= $(TOP)/traces/coremark_1m
check-real: all
	./build/bstfdump $(REAL) 6 --lax
	BSTF_LAX=1 ./build/t_refill   $(REAL)
	BSTF_LAX=1 ./build/t_end2end  $(REAL) $(REAL)
	$(MAKE) -C equiv build/equiv_mock
	BSTF_LAX=1 ./equiv/build/equiv_mock 200000 $(REAL)

clean:
	rm -rf build
	$(MAKE) -C equiv clean

distclean: clean
	rm -rf traces

.PHONY: all test bench equiv check-real clean distclean
