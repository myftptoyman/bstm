#include "tap.h"
#include "batch.h"
#include <string.h>
int main(void){
    enum { NSP=3, NCFG=100 };
    static bstf_trace_t tr[NSP];
    const bstf_trace_t *tp[NSP];
    bstm_instance_t *inst = calloc(NSP*NCFG,sizeof *inst);
    bstm_batchset_t bs; int i,s,c,n=0,tot=0;
    for(i=0;i<NSP;i++) tp[i]=&tr[i];
    /* 故意打散順序，驗證 plan 會重新分組 */
    for(c=0;c<NCFG;c++) for(s=0;s<NSP;s++){
        inst[n].config_id=(uint32_t)c; inst[n].simpoint_id=(uint32_t)s;
        inst[n].cfg[BSTM_CFG_ROB]= (uint32_t)(16+ (c%4)*16);
        inst[n].cfg[BSTM_CFG_IQ] = 32; inst[n].cfg[BSTM_CFG_FETCH_W]=4;
        inst[n].cfg[BSTM_CFG_COMMIT_W]=(uint32_t)(2+(c%2)); n++;
    }
    CHECK(bstm_batch_plan(inst,n,tp,NSP,&bs)==0,"plan ok");
    CHECK(bs.n==NSP*2,"batches=%d (100 config / 64 lanes x 3 simpoint)",bs.n);
    for(i=0;i<bs.n;i++){
        const bstm_batch_t *B=&bs.b[i];
        int l; uint32_t sp=B->simpoint_id;
        CHECK(B->trace==&tr[sp],"batch %d trace",i);
        for(l=0;l<B->n_lanes;l++){
            CHECK(B->inst[l].simpoint_id==sp,"batch %d lane %d 同 simpoint",i,l);
            CHECK(((B->lane_mask>>l)&1)==1,"lane_mask bit %d",l);
            CHECK(B->cfg[l*BSTM_CFG_N+BSTM_CFG_ROB]==B->inst[l].cfg[BSTM_CFG_ROB],"cfg copy");
        }
        for(l=B->n_lanes;l<BSTM_LANES;l++)
            CHECK(((B->lane_mask>>l)&1)==0,"空 lane %d 必須是 0",l);
        tot+=B->n_lanes;
    }
    CHECK(tot==n,"所有 instance 都被排進去 %d/%d",tot,n);
    bstm_batch_dump(&bs,stdout);
    bstm_batchset_free(&bs); free(inst);
    TAP_DONE("batch");
}
