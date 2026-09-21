# bstm-cc —— BSTM 編譯鏈（PLAN 第 6 章）
PY      ?= python3
YOSYS   ?= docker run --rm -v $(CURDIR)/tests/data:/w -w /w hdlc/yosys:latest yosys
CC      ?= gcc
CFLAGS  ?= -O3 -march=native
BUILD   ?= build

.PHONY: test cosim bench bench-big netlists clean help

help:
	@echo "make test      —— 跑自測（pytest 或 unittest）"
	@echo "make cosim     —— 跟 Verilator 逐 cycle 對拍"
	@echo "make bench     —— chain（815 cell）效能對照：原型 / 無 liveness / 有 liveness"
	@echo "make bench-big —— 800 級 chain（20,015 cell）同上；會跑很久（原型那一份編譯要好幾分鐘）"
	@echo "make netlists  —— 用 docker yosys 重新產生測試網表"

test:
	@if $(PY) -c "import pytest" 2>/dev/null; then \
	   $(PY) -m pytest -q tests/test_bstm_cc.py; \
	 else \
	   echo "(沒有 pytest，改用 unittest)"; \
	   $(PY) -W ignore::ResourceWarning -m unittest discover -s tests -p "test_*.py"; \
	 fi

cosim:
	cd tests && $(PY) cosim.py ops ops.v
	cd tests && $(PY) cosim.py chain stage.v chain.v
	cd tests && $(PY) cosim.py memtest memtest.v

netlists:
	cd tests && $(PY) synth.py chain stage.v chain.v
	cd tests && $(PY) synth.py ops ops.v
	cd tests && $(PY) synth.py asyncrd asyncrd.v
	cd tests && $(PY) synth.py badmul badmul.v
	cd tests && $(PY) -c "import synth; synth.synth(['memtest.v'],'memtest',extra='memory_dff; opt_clean')"
	cd tests/data && $(PY) ../gen_chain.py 800 > chain_big.v
	cd tests && $(PY) synth.py chain chain_big.json.tmp 2>/dev/null || true
	@echo "chain_big 請用：cd tests && python3 -c \"import synth;synth.synth(['stage.v','chain_big.v'],'chain',out='chain_big.json')\""

bench: $(BUILD)
	@$(MAKE) --no-print-directory bench-one J=tests/data/chain.json NAME=chain

bench-big: $(BUILD)
	@$(MAKE) --no-print-directory bench-one J=tests/data/chain_big.json NAME=chain_big

bench-one:
	@mkdir -p $(BUILD)
	@$(PY) ../../prototype/gen.py $(J) chain > $(BUILD)/$(NAME)_proto.c 2>/dev/null; \
	 { echo '#include <stdint.h>'; echo '#define BSTM_LANES 64'; \
	   cat $(BUILD)/$(NAME)_proto.c tests/bench_main.c; } > $(BUILD)/$(NAME)_proto_b.c
	@./bstm-cc $(J) chain --no-alloc -q -o $(BUILD)/$(NAME)_noalloc.c
	@cat $(BUILD)/$(NAME)_noalloc.c tests/bench_main.c > $(BUILD)/$(NAME)_noalloc_b.c
	@./bstm-cc $(J) chain --stats -q -o $(BUILD)/$(NAME)_live.c
	@cat $(BUILD)/$(NAME)_live.c tests/bench_main.c > $(BUILD)/$(NAME)_live_b.c
	@for v in proto noalloc live; do \
	   printf "%-10s compile " $$v; \
	   /usr/bin/time -f "%e s" $(CC) $(CFLAGS) -o $(BUILD)/$(NAME)_$$v $(BUILD)/$(NAME)_$${v}_b.c 2>&1 | tail -1; \
	   printf "%-10s run     " $$v; $(BUILD)/$(NAME)_$$v; \
	 done

$(BUILD):
	@mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD) __pycache__ bstm_cc/__pycache__ tests/__pycache__
