#include "tap.h"
#include "stats.h"
#include <string.h>
static uint64_t rnd(void){static uint64_t s=99194853094755497ULL;s^=s<<13;s^=s>>7;s^=s<<17;return s;}
int main(void){
    enum { NC=11, W=48 };
    static vec_t bs[NC*W];
    uint64_t in[64*NC], out[64*NC];
    int i,l;
    for(i=0;i<64*NC;i++) in[i]=rnd()&((1ULL<<W)-1);
    bstm_stats_inject(in,NC,W,bs);
    bstm_stats_extract(bs,NC,W,out);
    for(l=0;l<64;l++) for(i=0;i<NC;i++)
        CHECK(in[l*NC+i]==out[l*NC+i],"lane %d cnt %d: %llx != %llx",
              l,i,(unsigned long long)in[l*NC+i],(unsigned long long)out[l*NC+i]);
    TAP_DONE("stats");
}
