#include "tap.h"
#include "refill.h"
#include "transpose.h"
#include <string.h>
/* wrong-path 切換：強迫 lane 0 在看到 FE_REDIRECT 時跳 shadow，走完再回來。 */
int main(int argc,char**argv){
    const char *base = argc>1?argv[1]:"traces/synth0";
    static bstf_trace_t tr; static bstm_refill_t rf; static bstm_fbwin_t win;
    const bstf_trace_t *tp[BSTM_LANES];
    int c, saw_wp=0, saw_back=0;
    if (bstf_open(&tr, base)) { fprintf(stderr,"open: %s\n",bstf_strerror()); return 1; }
    for(c=0;c<BSTM_LANES;c++) tp[c]=&tr;
    bstm_refill_init(&rf,tp,1);          /* 只開 lane 0 */
    for(c=0;c<20000;c++){
        vec_t take[3]={0,0,0}; uint8_t tk[BSTM_LANES]={0};
        vec_t redir=0, rsh=0;
        uint32_t duop0=0; int b, in_wp;
        bstm_refill_window(&rf,&win);
        if(!((win.valid[0]>>0)&1)) break;
        for(b=0;b<32;b++) duop0 |= (uint32_t)((win.duop[b]>>0)&1)<<b;
        in_wp = (duop0>>31)&1;
        if(in_wp) saw_wp++;
        tk[0]=2; bstm_pack_lanes_small(tk,take,3);
        /* 每 300 cycle 觸發一次 shadow，之後 40 cycle 回來 */
        if(c%300==100){ redir=1; rsh=1; }
        if(c%300==140){ redir=1; rsh=0; if(saw_wp) saw_back=1; }
        bstm_refill_consume(&rf,take,redir,rsh);
    }
    CHECK(saw_wp>0,"進入過 wrong-path（看到 duop[31]=1）: %d 筆",saw_wp);
    CHECK(saw_back,"redirect 回正確路徑");
    CHECK(rf.n_redirect>0,"redirect 次數 %llu",(unsigned long long)rf.n_redirect);
    printf("  wrongpath uops seen on lane0: %d, redirects %llu\n",
           saw_wp,(unsigned long long)rf.n_redirect);
    bstf_close(&tr);
    TAP_DONE("refill wrong-path");
}
