#include "Vchain.h"
#include <cstdio>
#include <cstdint>
#include <ctime>
int main(){
    Vchain* d = new Vchain;
    uint64_t lfsr=0x123456789abcdefULL; volatile uint64_t sink=0;
    const long N=2000000;
    timespec t0,t1; clock_gettime(CLOCK_MONOTONIC,&t0);
    for(long i=0;i<N;i++){
        lfsr=(lfsr<<1)^(-(uint64_t)(lfsr>>63)&0x1B);
        d->rst=0; d->in_valid=lfsr&1; d->in_tag=(lfsr>>1)&63;
        d->in_lat=(lfsr>>7)&15; d->dn_credit_ret=(lfsr>>11)&1;
        d->clk=0; d->eval(); d->clk=1; d->eval();
        sink ^= d->in_ready ^ d->out_valid ^ d->out_tag;
    }
    clock_gettime(CLOCK_MONOTONIC,&t1);
    double s=(t1.tv_sec-t0.tv_sec)+(t1.tv_nsec-t0.tv_nsec)/1e9;
    printf("Verilator  : %.2f M target-cycles/s  |  %.2f M instance-cycles/s (1 instance, 單核)  (sink %llx)\n",
           N/s/1e6, N/s/1e6, (unsigned long long)sink);
    return 0;
}
