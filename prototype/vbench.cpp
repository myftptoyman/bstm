#include "Vstage.h"
#include <cstdio>
#include <cstdint>
#include <ctime>
int main(){
    Vstage* d = new Vstage;
    uint64_t lfsr = 0x123456789abcdefULL;
    volatile uint64_t sink=0;
    const long N = 20000000;
    timespec t0,t1; clock_gettime(CLOCK_MONOTONIC,&t0);
    for(long i=0;i<N;i++){
        lfsr = (lfsr<<1) ^ (-(uint64_t)(lfsr>>63) & 0x1B);
        d->rst=0; d->in_valid = lfsr&1; d->in_tag=(lfsr>>1)&63;
        d->in_lat=(lfsr>>7)&15; d->dn_credit_ret=(lfsr>>11)&1;
        d->clk=0; d->eval();
        d->clk=1; d->eval();
        sink ^= d->in_ready ^ d->out_valid ^ d->out_tag;
    }
    clock_gettime(CLOCK_MONOTONIC,&t1);
    double s=(t1.tv_sec-t0.tv_sec)+(t1.tv_nsec-t0.tv_nsec)/1e9;
    printf("Verilator: %ld target-cycles in %.3f s  (sink %llx)\n",N,s,(unsigned long long)sink);
    printf("  單 instance : %.1f M target-cycles/s\n", N/s/1e6);
    return 0;
}
