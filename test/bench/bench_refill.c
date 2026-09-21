/* refill benchmark：每 target cycle 的攤銷成本（ns 與 host cycle）
 * 並與 mock 模型本體的 eval 成本對照，算出 refill 佔總成本的比例。 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "refill.h"
#include "transpose.h"
#include "mock_ooo_bs.h"

static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    return t.tv_sec+t.tv_nsec/1e9;}

/* 量測 CPU 的實際頻率（用 rdtsc 不可靠，這裡用一個已知 op 數的迴圈估） */
static double est_ghz(void){
    volatile uint64_t x=1; long i; double t0=now();
    for(i=0;i<300000000L;i++) x = x*6364136223846793005ULL + 1;  /* 3 cycle/iter (imul lat) */
    double t1=now();
    return 300000000.0*3.0/(t1-t0)/1e9;
}

int main(int argc,char**argv){
    const char *base = argc>1?argv[1]:"traces/synth0";
    static bstf_trace_t tr;
    static bstm_refill_t rf;
    static bstm_fbwin_t win;
    const bstf_trace_t *tp[BSTM_LANES];
    const long N = 400000;
    long i; double t0,t1,tr_ns,ev_ns,ghz;
    vec_t take[3]; uint8_t tk[BSTM_LANES];
    mock_state_t *a,*b,*t; bstm_out_t out;
    uint32_t cfg[BSTM_LANES*BSTM_CFG_N];
    int l,k;
    uint64_t tot_gather=0, tot_rec=0;

    if (bstf_open(&tr,base)) { fprintf(stderr,"open %s failed: %s\n",base,bstf_strerror()); return 1; }
    for(l=0;l<BSTM_LANES;l++) tp[l]=&tr;

    ghz = est_ghz();
    printf("estimated clock: %.2f GHz\n\n", ghz);

    /* ---- refill only ---- */
    bstm_refill_init(&rf,tp,~(uint64_t)0);
    for(l=0;l<BSTM_LANES;l++) tk[l]=(uint8_t)(1+(l&3));
    bstm_pack_lanes_small(tk,take,3);
    /* trace 跑完就重開，避免量到「所有 lane 都 eof」的空轉。
     * 重開會清掉 rf 的統計，所以自己累加。 */
    { uint64_t g=0,gr=0; int reinit=0;
      t0=now();
      for(i=0;i<N;i++){
        if((i & 255)==0 && bstm_refill_done_mask(&rf)==~(uint64_t)0){
            g+=rf.n_gather; gr+=rf.n_gathered; reinit++;
            bstm_refill_init(&rf,tp,~(uint64_t)0);
        }
        bstm_refill_window(&rf,&win);
        bstm_refill_consume(&rf,take,VZERO,VZERO);
      }
      t1=now(); tr_ns=(t1-t0)/N*1e9;
      g+=rf.n_gather; gr+=rf.n_gathered;
      tot_gather=g; tot_rec=gr;
      printf("  (trace 跑完重開 %d 次；每 lane 固定 take = 1..4，平均 2.5，"
             "是最壞情況)\n",reinit); }
    { double w0,w1; long j;
      bstm_refill_init(&rf,tp,~(uint64_t)0);
      w0=now();
      for(j=0;j<N;j++){
        if((j & 255)==0 && bstm_refill_done_mask(&rf)==~(uint64_t)0)
            bstm_refill_init(&rf,tp,~(uint64_t)0);
        bstm_refill_window(&rf,&win);
      }
      w1=now();
      printf("  拆解: window(純轉置，不消耗) %6.1f ns  |  "
             "consume + scalar gather %6.1f ns\n",
             (w1-w0)/N*1e9, tr_ns-(w1-w0)/N*1e9); }
    printf("refill (window+consume) : %7.1f ns / target cycle  = %5.1f host cycles\n",
           tr_ns, tr_ns*ghz);
    printf("                          %7.3f ns / instance-cycle (64 lane)\n", tr_ns/64);
    printf("  gather calls %llu = 每 lane 每 %.1f cycle 一次 (REFILL_M=%d)\n",
           (unsigned long long)tot_gather,
           (double)N*BSTM_LANES/(double)tot_gather, BSTM_REFILL_M);
    printf("  gathered %llu records = %.2f rec/cycle/lane\n\n",
           (unsigned long long)tot_rec,
           (double)tot_rec/(double)N/BSTM_LANES);

    /* ---- model eval only ---- */
    a=calloc(1,sizeof *a); b=calloc(1,sizeof *b);
    for(l=0;l<BSTM_LANES;l++){
        cfg[l*BSTM_CFG_N+BSTM_CFG_ROB]=64; cfg[l*BSTM_CFG_N+BSTM_CFG_IQ]=32;
        cfg[l*BSTM_CFG_N+BSTM_CFG_LDQ]=16; cfg[l*BSTM_CFG_N+BSTM_CFG_STQ]=16;
        cfg[l*BSTM_CFG_N+BSTM_CFG_MSHR]=8; cfg[l*BSTM_CFG_N+BSTM_CFG_FETCH_W]=4;
        cfg[l*BSTM_CFG_N+BSTM_CFG_ISSUE_W]=4; cfg[l*BSTM_CFG_N+BSTM_CFG_COMMIT_W]=2;
    }
    bstm_mock_model.init(a,cfg,~(uint64_t)0);
    bstm_mock_model.init(b,cfg,~(uint64_t)0);
    t0=now();
    for(i=0;i<N;i++){ mock_eval_rst(b,a,&win,&out,VZERO); t=a;a=b;b=t; }
    t1=now(); ev_ns=(t1-t0)/N*1e9;
    printf("mock model eval         : %7.1f ns / target cycle  = %5.1f host cycles\n",
           ev_ns, ev_ns*ghz);
    printf("  (mock 只有 ~%d 個 bit 的狀態 + 11 個 48-bit counter，真模型會大很多)\n\n",
           7+1+6);

    /* ---- 合起來 ---- */
    printf("refill / (refill+eval)  : %5.1f %%\n", 100.0*tr_ns/(tr_ns+ev_ns));
    printf("合併吞吐                : %.1f M instance-cycles/s (單核)\n",
           64.0/(tr_ns+ev_ns)*1e9/1e6);
    (void)k;
    bstf_close(&tr);
    return 0;
}
