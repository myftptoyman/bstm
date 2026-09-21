# ============================================================
# BSTM 離線前端模擬器（Agent A）— preprocess/frontend
# ============================================================
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter
BUILD    := build

BINS := $(BUILD)/frontend_sim $(BUILD)/fe_check

.PHONY: all clean test check demo sweep

all: $(BINS)

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/frontend_sim: frontend_sim.cpp json.hpp predictors.hpp sha256.hpp ../../include/bstf.h | $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ frontend_sim.cpp

$(BUILD)/fe_check: fe_check.cpp ../../include/bstf.h | $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ fe_check.cpp

# 產生合成指令串流（巢狀迴圈 + 函式呼叫 + 週期性 pattern）
$(BUILD)/loop.trace: gen_trace.py | $(BUILD)
	python3 gen_trace.py 400 20 > $@

# 壓力串流：深度 20 的呼叫鏈（超過 RAS 16 entry）+ 輪替目標的 jalr
$(BUILD)/hard.trace: gen_trace.py | $(BUILD)
	python3 gen_trace.py --hard 2000 > $@

demo: all $(BUILD)/loop.trace
	$(BUILD)/frontend_sim $(BUILD)/loop.trace config.json $(BUILD)/loop.fe

test: all $(BUILD)/loop.trace $(BUILD)/hard.trace
	@./selftest.sh

# wrongpath_depth (D) 敏感度掃描
sweep: all $(BUILD)/loop.trace $(BUILD)/hard.trace
	@./sweep_d.sh $(BUILD)/loop.trace 0 4 8 10 16 24 32
	@echo
	@./sweep_d.sh $(BUILD)/hard.trace 0 4 8 10 12 14 16 24 32

clean:
	rm -rf $(BUILD)
