#include "tap.h"
#include "refill.h"
#include "transpose.h"
#include <string.h>

/* 用 scalar 參考實作重跑一遍，確認 bit-sliced window 與逐 lane 純量結果一致。 */
int main(int argc,char**argv){
    const char *base = argc>1?argv[1]:"traces/synth0";
    static bstf_trace_t tr;
    static bstm_refill_t rf;
    static bstm_fbwin_t win;
    const bstf_trace_t *tp[BSTM_LANES];
    bstf_cursor_t ref[BSTM_LANES];
    int l,c,e,b;
    if (bstf_open(&tr, base)) { fprintf(stderr,"open: %s\n",bstf_strerror()); return 1; }
    for(l=0;l<BSTM_LANES;l++) tp[l]=&tr;
    bstm_refill_init(&rf,tp,~(uint64_t)0);
    for(l=0;l<BSTM_LANES;l++) bstf_cur_init(&tr,&ref[l]);

    /* 每 lane 用不同的固定 take，模擬不同 config 的消耗速率 */
    for(c=0;c<3000;c++){
        vec_t take[3]; uint8_t tk[BSTM_LANES];
        bstm_refill_window(&rf,&win);
        /* 比對 window 內容 */
        for(l=0;l<BSTM_LANES;l++){
            bstf_cursor_t p = ref[l];
            for(e=0;e<BSTM_W;e++){
                const bstf_rec_t *r = bstf_peek(&tr,&p);
                int gv = (int)((win.valid[e]>>l)&1);
                uint32_t gd=0, gm=0, want_d, want_m;
                for(b=0;b<32;b++) gd |= (uint32_t)((win.duop[e*32+b]>>l)&1)<<b;
                for(b=0;b<8;b++)  gm |= (uint32_t)((win.mem[e*8+b]>>l)&1)<<b;
                if(!r){ if(gv){CHECK(0,"lane %d e %d should be invalid",l,e);goto out;} continue; }
                if(!gv){ CHECK(0,"lane %d e %d should be valid (c=%d)",l,e,c); goto out; }
                want_d = bstf_rec_to_duop(r,p.in_shadow);
                want_m = bstf_mem_event_of(&tr,&p,r);   /* 非 mem 指令 -> 0 */
                if(gd!=want_d){CHECK(0,"lane %d e %d duop %08x != %08x",l,e,gd,want_d);goto out;}
                if(gm!=want_m){CHECK(0,"lane %d e %d mem %02x != %02x",l,e,gm,want_m);goto out;}
                bstf_advance(&tr,&p,1);
            }
            { uint32_t gf=0, wf; const bstf_rec_t *r0=bstf_peek(&tr,&ref[l]);
              for(b=0;b<8;b++) gf |= (uint32_t)((win.fe[b]>>l)&1)<<b;
              wf = r0 ? bstf_fe_event(&tr,&ref[l]) : 0;
              if(gf!=wf){CHECK(0,"lane %d fe %02x != %02x",l,gf,wf);goto out;} }
        }
        for(l=0;l<BSTM_LANES;l++){
            tk[l] = (uint8_t)(1 + (l & 3));          /* 1..4 */
            bstf_advance(&tr,&ref[l],tk[l]);
        }
        bstm_pack_lanes_small(tk,take,3);
        bstm_refill_consume(&rf,take,VZERO,VZERO);
    }
out:
    CHECK(tap_fail==0,"window matches scalar reference");
    printf("  refill: %llu cycles, %llu gather calls, %llu records (%.2f rec/cycle/lane)\n",
           (unsigned long long)rf.n_cycles,(unsigned long long)rf.n_gather,
           (unsigned long long)rf.n_gathered,
           (double)rf.n_gathered/(double)rf.n_cycles/BSTM_LANES);
    bstf_close(&tr);
    TAP_DONE("refill");
}
