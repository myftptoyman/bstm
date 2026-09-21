/* 端到端：mmap trace -> batch 打包 -> thread pool -> mock 模型 -> CSV
 * 這一條路徑在真模型到位之前就把 runtime 全部走過一遍。 */
#include "tap.h"
#include "pool.h"
#include "stats.h"
#include "mock_ooo_bs.h"
#include <string.h>
#include <time.h>

typedef struct { const bstm_model_t *m; bstm_run_opt_t opt; } ctx_t;

static void run_job(bstm_batch_t *b, void *cv, int id)
{
    ctx_t *c = cv;
    (void)id;
    b->counters = calloc(BSTM_LANES * MOCK_NCNT, sizeof(uint64_t));
    bstm_run_batch(c->m, b, &c->opt, b->counters);
}

int main(int argc, char **argv)
{
    const char *t0 = argc>1?argv[1]:"traces/synth0";
    const char *t1 = argc>2?argv[2]:"traces/synth1";
    static bstf_trace_t tr[2];
    const bstf_trace_t *tp[2] = { &tr[0], &tr[1] };
    bstm_instance_t *inst; bstm_batchset_t bs; bstm_pool_t *pool;
    ctx_t ctx; int i,s,c,n=0; FILE *fp; struct timespec a,z; double el;
    uint64_t tot_cycles=0, tot_ret=0;
    enum { NCFG = 100, NSP = 2 };

    if (bstf_open(&tr[0],t0) || bstf_open(&tr[1],t1)) {
        fprintf(stderr,"open trace: %s\n",bstf_strerror()); return 1; }

    inst = calloc(NCFG*NSP,sizeof *inst);
    for(s=0;s<NSP;s++) for(c=0;c<NCFG;c++){
        inst[n].config_id=(uint32_t)c; inst[n].simpoint_id=(uint32_t)s;
        inst[n].cfg[BSTM_CFG_ROB]      = (uint32_t)(16 + (c%5)*12);   /* 16..64 */
        inst[n].cfg[BSTM_CFG_IQ]       = (uint32_t)(8  + (c%4)*8);
        inst[n].cfg[BSTM_CFG_LDQ]      = 16; inst[n].cfg[BSTM_CFG_STQ]=16;
        inst[n].cfg[BSTM_CFG_MSHR]     = 8;
        inst[n].cfg[BSTM_CFG_FETCH_W]  = (uint32_t)(2 + (c%3));       /* 2..4 */
        inst[n].cfg[BSTM_CFG_ISSUE_W]  = 4;
        inst[n].cfg[BSTM_CFG_COMMIT_W] = (uint32_t)(1 + (c%4));       /* 1..4 */
        n++;
    }
    CHECK(bstm_batch_plan(inst,n,tp,NSP,&bs)==0,"plan");
    CHECK(bs.n==4,"batches=%d",bs.n);

    ctx.m = &bstm_mock_model;
    ctx.opt.max_cycles = 2000000; ctx.opt.done_check_mask = 255;
    pool = bstm_pool_create(8);
    clock_gettime(CLOCK_MONOTONIC,&a);
    bstm_pool_run(pool,bs.b,bs.n,run_job,&ctx);
    clock_gettime(CLOCK_MONOTONIC,&z);
    el=(z.tv_sec-a.tv_sec)+(z.tv_nsec-a.tv_nsec)/1e9;

    fp = fopen("build/sweep_mock.csv","w");
    if(!fp) fp = fopen("sweep_mock.csv","w");
    bstm_stats_csv_header(fp,mock_counter_names,MOCK_NCNT);
    for(i=0;i<bs.n;i++){
        bstm_stats_csv_batch(fp,&bs.b[i],mock_counter_names,MOCK_NCNT,
                             bs.b[i].counters,0,1);
        tot_cycles += bs.b[i].target_cycles;
    }
    fclose(fp);

    /* 合理性：每個 lane 的 retired <= cycles*commit_width，且 > 0 */
    for(i=0;i<bs.n;i++){
        const bstm_batch_t *B=&bs.b[i]; int l;
        for(l=0;l<B->n_lanes;l++){
            const uint64_t *cn = B->counters + (size_t)l*MOCK_NCNT;
            uint32_t cw = B->inst[l].cfg[BSTM_CFG_COMMIT_W];
            if(cn[0]==0){ CHECK(0,"batch %d lane %d 零 cycle",i,l); continue; }
            if(cn[1]==0){ CHECK(0,"batch %d lane %d 零 retire",i,l); continue; }
            if(cn[1] > cn[0]*cw){ CHECK(0,"batch %d lane %d retire %llu > cycles*cw %llu",
                i,l,(unsigned long long)cn[1],(unsigned long long)(cn[0]*cw)); continue; }
            /* 先跑完的 lane 會被 set_enable 凍結，所以 cnt_cycles <= batch 的總 cycle */
            if(cn[0] > B->target_cycles){ CHECK(0,"batch %d lane %d cnt_cycles %llu > %llu",
                i,l,(unsigned long long)cn[0],(unsigned long long)B->target_cycles); continue; }
            tot_ret += cn[1];
        }
    }
    CHECK(tap_fail==0,"所有 lane 的統計合理");
    printf("  end2end: %d batch x 64 lane, %llu target-cycles, %.3f s\n",
           bs.n,(unsigned long long)tot_cycles,el);
    printf("           %.1f M instance-cycles/s (8 worker), CSV -> build/sweep_mock.csv\n",
           (double)tot_cycles*200/el/1e6);
    bstm_pool_destroy(pool);
    bstm_batchset_free(&bs); free(inst);
    bstf_close(&tr[0]); bstf_close(&tr[1]);
    TAP_DONE("end2end mock");
}
