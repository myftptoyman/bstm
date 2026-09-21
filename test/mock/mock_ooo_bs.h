/* mock_ooo_bs.h — mock_ooo_top.v 的手寫 bit-sliced 版本（64 lane）。
 * 真正的版本應該由 tools/bstm-cc 從 Verilog 產生；在 Agent C 的編譯器
 * 完成之前，這份手寫版讓 runtime 可以端到端跑，也當 test/equiv 的
 * 第二個對拍目標（Verilator mock vs 這份 C）。 */
#ifndef MOCK_OOO_BS_H
#define MOCK_OOO_BS_H

#include "bstm_types.h"
#include "bstm_abi.h"

#define MOCK_NCNT 11

typedef struct {
    vec_t occ[7];
    vec_t in_shadow;
    vec_t shadow_cnt[6];
    vec_t cnt[MOCK_NCNT][BSTM_CNT_W];
    vec_t cfg[BSTM_CFG_N][8];
    vec_t enable;
} mock_state_t;

extern const bstm_model_t bstm_mock_model;
extern const char *const  mock_counter_names[MOCK_NCNT];

/* 低階入口：多一個 rst（ABI 的 eval() 固定用 rst=0）。 */
void mock_eval_rst(mock_state_t *nxt, const mock_state_t *cur,
                   const bstm_fbwin_t *win, bstm_out_t *out, vec_t rst);

#endif
