# test/equiv — Verilator <-> bit-sliced 逐 cycle 對拍（PLAN §9.2）
TOP       := $(abspath ../..)
RT        := $(TOP)/lib/runtime
PROTO     := $(TOP)/prototype
VERILATOR ?= verilator
CC        ?= gcc

# 關鍵設定：沒有這兩個 flag，X 語意差異會製造大量假警報
XFLAGS  := --x-assign 0 --x-initial 0
VFLAGS  := --cc --exe --build -O3 $(XFLAGS) -Wno-fatal \
           -CFLAGS "-O2 -std=c++17 -I$(CURDIR) -I$(RT) -I$(TOP)/include"

N ?= 200000

all: build/equiv_chain build/equiv_mock

build:
	@mkdir -p build

# ---------------- chain.v ----------------
build/equiv_chain: equiv_chain.cpp equiv.h gen/chain_bs.c gen/chain_bs.h | build
	$(VERILATOR) $(VFLAGS) --Mdir build/obj_chain --top-module chain \
	    $(PROTO)/chain.v $(PROTO)/stage.v \
	    $(CURDIR)/equiv_chain.cpp $(CURDIR)/gen/chain_bs.c -o $(CURDIR)/build/equiv_chain

# ---------------- mock_ooo_top.v ----------------
# mock 的 bit-sliced 版本是 C11，單獨用 gcc 編（Verilator 會把 .c 當 C++ 編）
build/mock_ooo_bs.o: ../mock/mock_ooo_bs.c | build
	$(CC) -O3 -std=c11 -Wall -Wextra -I$(RT) -I$(TOP)/include -I$(TOP)/test/mock \
	    -c $< -o $@

build/equiv_mock: equiv_mock.cpp equiv.h ../mock/mock_ooo_top.v build/mock_ooo_bs.o $(RT)/libbstm.a | build
	$(MAKE) -C $(RT)
	$(VERILATOR) $(VFLAGS) -CFLAGS "-I$(TOP)/test/mock" \
	    -LDFLAGS "$(CURDIR)/build/mock_ooo_bs.o $(RT)/libbstm.a -pthread" \
	    --Mdir build/obj_mock --top-module mock_ooo_top \
	    +incdir+$(TOP)/model/ooo $(TOP)/test/mock/mock_ooo_top.v \
	    $(CURDIR)/equiv_mock.cpp -o $(CURDIR)/build/equiv_mock

run: all
	@cd $(TOP)/test && ./equiv/build/equiv_chain $(N) traces/synth0.bstf
	@cd $(TOP)/test && ./equiv/build/equiv_mock  $(N) traces/synth0

check: run

clean:
	rm -rf build

.PHONY: all run check clean
