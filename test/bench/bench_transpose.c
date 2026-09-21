/* transpose benchmark：ns / 64x64 transpose，以及小 n 特化的比較 */
#include <stdio.h>
#include <time.h>
#include "transpose.h"
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    return t.tv_sec+t.tv_nsec/1e9;}
static uint64_t rnd(void){static uint64_t s=88172645463325252ULL;s^=s<<13;s^=s>>7;s^=s<<17;return s;}
int main(void){
    static uint64_t A[64]; uint64_t sink=0; long i; double t0,t1;
    const long N = 2000000;
    for(i=0;i<64;i++)A[i]=rnd();

    t0=now();
    for(i=0;i<N;i++){ bstm_transpose64(A); A[0]^=(uint64_t)i; }
    t1=now(); sink^=A[3];
    printf("transpose64 (in-place)   : %7.2f ns/次  (%.2f ns / 64-bit row, %.3f ns/bit-lane)\n",
           (t1-t0)/N*1e9, (t1-t0)/N*1e9/64, (t1-t0)/N*1e9/4096);

    { static uint64_t B[64];
      t0=now();
      for(i=0;i<N;i++){ bstm_transpose64_to(B,A); A[0]^=B[1]; }
      t1=now(); sink^=B[5];
      printf("transpose64_to (copy)    : %7.2f ns/次\n",(t1-t0)/N*1e9); }

    { uint64_t v[64]; vec_t w[48];
      for(i=0;i<64;i++)v[i]=rnd();
      t0=now();
      for(i=0;i<N;i++){ bstm_pack_lanes(v,w,48); v[0]^=w[0]; }
      t1=now(); sink^=w[7];
      printf("pack_lanes(48 bit)       : %7.2f ns/次\n",(t1-t0)/N*1e9); }

    { vec_t w[3]={1,2,3}; uint8_t o[64];
      t0=now();
      for(i=0;i<N;i++){ bstm_unpack_lanes_small(w,o,3); w[0]^=o[3]; }
      t1=now(); sink^=o[9];
      printf("unpack_lanes_small(3bit) : %7.2f ns/次  (fb_take 每 cycle 要做一次)\n",
             (t1-t0)/N*1e9); }

    { vec_t w[3]; uint64_t v[64];
      for(i=0;i<64;i++)v[i]=rnd()&7;
      t0=now();
      for(i=0;i<N;i++){ bstm_pack_lanes(v,w,3); v[0]^=w[0]; }
      t1=now(); sink^=w[1];
      printf("pack_lanes(3 bit, 完整轉置): %7.2f ns/次  (對照組)\n",(t1-t0)/N*1e9); }

    printf("(sink %llx)\n",(unsigned long long)sink);
    return 0;
}
