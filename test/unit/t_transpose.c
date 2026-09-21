#include "tap.h"
#include "transpose.h"
static uint64_t rnd(void){static uint64_t s=88172645463325252ULL;s^=s<<13;s^=s>>7;s^=s<<17;return s;}
int main(void){
    uint64_t A[64],B[64],C[64],v[64],v2[64];
    vec_t w[64]; uint8_t s8[64],t8[64];
    int i,j,b;
    for(i=0;i<64;i++)A[i]=rnd();
    bstm_transpose64_to(B,A);
    for(i=0;i<64;i++)for(j=0;j<64;j++)
        CHECK(((B[i]>>j)&1)==((A[j]>>i)&1),"i=%d j=%d",i,j);
    bstm_transpose64_to(C,B);
    for(i=0;i<64;i++) CHECK(C[i]==A[i],"involution i=%d",i);

    /* 單一 bit 的定位 */
    for(i=0;i<64;i++)A[i]=0;
    A[5]=1ULL<<9; bstm_transpose64_to(B,A);
    CHECK(B[9]==(1ULL<<5),"single bit -> B[9]=%llx",(unsigned long long)B[9]);

    for(i=0;i<64;i++) v[i]=rnd()&((1ULL<<48)-1);
    bstm_pack_lanes(v,w,48); bstm_unpack_lanes(w,v2,48);
    for(i=0;i<64;i++) CHECK(v[i]==v2[i],"pack/unpack 48 lane %d",i);

    for(i=0;i<64;i++) s8[i]=(uint8_t)(rnd()&7);
    bstm_pack_lanes_small(s8,w,3); bstm_unpack_lanes_small(w,t8,3);
    for(i=0;i<64;i++) CHECK(s8[i]==t8[i],"small pack lane %d",i);
    for(i=0;i<64;i++) v[i]=s8[i];
    { vec_t wf[3]; bstm_pack_lanes(v,wf,3);
      for(b=0;b<3;b++) CHECK(wf[b]==w[b],"small vs full bit %d",b); }

    /* multi */
    { uint64_t mv[64*4]; vec_t mo[4*48]; 
      for(i=0;i<64*4;i++) mv[i]=rnd()&((1ULL<<48)-1);
      bstm_pack_lanes_multi(mv,4,mo,48);
      for(i=0;i<64;i++) for(j=0;j<4;j++) for(b=0;b<48;b++)
        if((((mo[j*48+b]>>i)&1))!=((mv[i*4+j]>>b)&1)){CHECK(0,"multi %d %d %d",i,j,b);goto done;}
      done: ; }
    TAP_DONE("transpose");
}
