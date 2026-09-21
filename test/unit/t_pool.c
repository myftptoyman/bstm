#include "tap.h"
#include "pool.h"
#include <string.h>
#include <stdatomic.h>
static atomic_ullong sum;
static atomic_int    maxpar, curpar;
static void job(bstm_batch_t *b,void *ctx,int id){
    int p = atomic_fetch_add(&curpar,1)+1, m;
    volatile double x=0; long i;
    (void)ctx;(void)id;
    do { m = atomic_load(&maxpar); } while(p>m && !atomic_compare_exchange_weak(&maxpar,&m,p));
    for(i=0;i<200000;i++) x += (double)i*0.5;     /* 讓工作量不均 */
    if(b->simpoint_id & 1) for(i=0;i<400000;i++) x += (double)i*0.25;
    atomic_fetch_add(&sum,(unsigned long long)b->simpoint_id);
    atomic_fetch_sub(&curpar,1);
    b->target_cycles = (uint64_t)x & 1;
}
int main(void){
    enum { NB=200, NW=8 };
    bstm_batch_t *b = calloc(NB,sizeof *b);
    bstm_pool_t *p; uint64_t done[NW],stolen[NW],tot=0,st=0,want=0; int i;
    for(i=0;i<NB;i++){ b[i].simpoint_id=(uint32_t)i; want+=(uint64_t)i; }
    p = bstm_pool_create(NW);
    CHECK(bstm_pool_nworkers(p)==NW,"nworkers");
    bstm_pool_run(p,b,NB,job,NULL);
    bstm_pool_stats(p,done,stolen);
    for(i=0;i<NW;i++){ tot+=done[i]; st+=stolen[i]; }
    CHECK(tot==NB,"每個 batch 剛好跑一次: %llu/%d",(unsigned long long)tot,NB);
    CHECK(atomic_load(&sum)==want,"聚合結果正確");
    CHECK(atomic_load(&maxpar)>1,"真的有平行: maxpar=%d",atomic_load(&maxpar));
    printf("  pool: %d workers, %llu batches, %llu stolen\n",NW,
           (unsigned long long)tot,(unsigned long long)st);
    /* 第二輪：確認 pool 可重複使用 */
    atomic_store(&sum,0);
    bstm_pool_run(p,b,NB,job,NULL);
    CHECK(atomic_load(&sum)==want,"第二輪也正確");
    bstm_pool_destroy(p); free(b);
    TAP_DONE("pool");
}
