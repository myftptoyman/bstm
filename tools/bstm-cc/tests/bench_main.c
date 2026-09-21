/* chain 系列（stage.v 的埠）用的 benchmark 驅動。跟 prototype/cbench.c 的 main
 * 完全一樣的刺激序列與 checksum，所以新舊產生器可以直接對數字。 */
#include <stdio.h>
#include <time.h>
#ifndef BENCH_N
#define BENCH_N 2000000
#endif
int main(void){
    static state_t a,b; vec_t rdy,ov,tag[6],it[6],il[4];
    volatile vec_t sink=0; vec_t lfsr=0x123456789abcdefULL;
    const long N=BENCH_N;
    struct timespec t0,t1; clock_gettime(CLOCK_MONOTONIC,&t0);
    for(long i=0;i<N;i++){
        lfsr=(lfsr<<1)^(-(vec_t)(lfsr>>63)&0x1B);
        for(int k=0;k<6;k++) it[k]=lfsr*(k+1);
        for(int k=0;k<4;k++) il[k]=lfsr>>(k+3);
        eval_cycle(&b,&a,VZERO,lfsr,it,il,lfsr>>7,&rdy,&ov,tag);
        a=b; sink^=rdy^ov^tag[0];
    }
    clock_gettime(CLOCK_MONOTONIC,&t1);
    double s=(t1.tv_sec-t0.tv_sec)+(t1.tv_nsec-t0.tv_nsec)/1e9;
    printf("%8.3f M target-cycles/s | %9.1f M instance-cycles/s | sink %016llx\n",
           N/s/1e6, N*(double)BSTM_LANES/s/1e6,(unsigned long long)sink);
    return 0;
}
