#include <stdint.h>
typedef uint64_t vec_t;        /* 64 lanes = 64 instances */
#define VZERO ((vec_t)0)
#define VONES (~(vec_t)0)

typedef struct {   /* 所有 DFF 的狀態，每個 bit 一個 word */
    vec_t q18;
    vec_t q19;
    vec_t q20;
    vec_t q21;
    vec_t q22;
    vec_t q23;
    vec_t q24;
    vec_t q25;
    vec_t q26;
    vec_t q27;
    vec_t q32;
    vec_t q49;
    vec_t q50;
    vec_t q51;
    vec_t q52;
} state_t;

static void eval_cycle(state_t *s, const state_t *p,
                       vec_t rst, vec_t in_valid, const vec_t in_tag[6],
                       const vec_t in_lat[4], vec_t dn_credit_ret,
                       vec_t *in_ready, vec_t *out_valid, vec_t out_tag[6])
{
    vec_t n2;
    vec_t n3;
    vec_t n4;
    vec_t n5;
    vec_t n6;
    vec_t n7;
    vec_t n8;
    vec_t n9;
    vec_t n10;
    vec_t n11;
    vec_t n12;
    vec_t n13;
    vec_t n14;
    vec_t n15;
    vec_t n16;
    vec_t n17;
    vec_t n18;
    vec_t n19;
    vec_t n20;
    vec_t n21;
    vec_t n22;
    vec_t n23;
    vec_t n24;
    vec_t n25;
    vec_t n26;
    vec_t n27;
    vec_t n28;
    vec_t n29;
    vec_t n30;
    vec_t n31;
    vec_t n32;
    vec_t n33;
    vec_t n34;
    vec_t n35;
    vec_t n36;
    vec_t n37;
    vec_t n38;
    vec_t n39;
    vec_t n40;
    vec_t n41;
    vec_t n42;
    vec_t n43;
    vec_t n44;
    vec_t n45;
    vec_t n46;
    vec_t n47;
    vec_t n48;
    vec_t n49;
    vec_t n50;
    vec_t n51;
    vec_t n52;
    vec_t n53;
    vec_t n54;
    vec_t n55;
    vec_t n56;
    vec_t n57;
    vec_t n58;
    vec_t n59;
    vec_t n60;
    vec_t n61;
    vec_t n62;
    vec_t n63;
    vec_t n64;
    vec_t n65;
    vec_t n66;
    vec_t n67;
    vec_t n68;
    vec_t n69;

    /* --- FF 輸出 = 上一 cycle 的狀態 --- */
    n18 = p->q18;
    n19 = p->q19;
    n20 = p->q20;
    n21 = p->q21;
    n22 = p->q22;
    n23 = p->q23;
    n24 = p->q24;
    n25 = p->q25;
    n26 = p->q26;
    n27 = p->q27;
    n32 = p->q32;
    n49 = p->q49;
    n50 = p->q50;
    n51 = p->q51;
    n52 = p->q52;

    /* --- 輸入接腳 --- */
    n3 = rst;
    n4 = in_valid;
    n5 = in_tag[0];
    n6 = in_tag[1];
    n7 = in_tag[2];
    n8 = in_tag[3];
    n9 = in_tag[4];
    n10 = in_tag[5];
    n11 = in_lat[0];
    n12 = in_lat[1];
    n13 = in_lat[2];
    n14 = in_lat[3];
    n15 = dn_credit_ret;

    /* --- 組合邏輯（已依拓樸順序排好） --- */
    {  /* add: ripple carry, 4 bit */
        vec_t c = VZERO;
        { vec_t a=n24, b=VONES;
          n28 = a ^ b ^ c;
          c = (a&b) | (c&(a^b)); }
        { vec_t a=n25, b=VZERO;
          n29 = a ^ b ^ c;
          c = (a&b) | (c&(a^b)); }
        { vec_t a=n26, b=VZERO;
          n30 = a ^ b ^ c;
          c = (a&b) | (c&(a^b)); }
        { vec_t a=n27, b=VZERO;
          n31 = a ^ b ^ c;
          c = (a&b) | (c&(a^b)); }
    }
    n35 = ~(n49 | n50 | n51 | n52);   /* logic_not (==0) */
    n33 = n49 | n50 | n51 | n52;   /* reduce_bool (!=0) */
    n37 = n24 | n25 | n26 | n27;   /* reduce_bool (!=0) */
    n16 = ~n32;
    {  /* sub: ripple carry, 4 bit */
        vec_t c = VONES;
        { vec_t a=n49, b=~VONES;
          n65 = a ^ b ^ c;
          c = (a&b) | (c&(a^b)); }
        { vec_t a=n50, b=VONES;
          n66 = a ^ b ^ c;
          c = (a&b) | (c&(a^b)); }
        { vec_t a=n51, b=VONES;
          n67 = a ^ b ^ c;
          c = (a&b) | (c&(a^b)); }
        { vec_t a=n52, b=VONES;
          n68 = a ^ b ^ c;
          c = (a&b) | (c&(a^b)); }
    }
    {  /* sub: ripple carry, 4 bit */
        vec_t c = VONES;
        { vec_t a=n24, b=~VONES;
          n57 = a ^ b ^ c;
          c = (a&b) | (c&(a^b)); }
        { vec_t a=n25, b=VONES;
          n58 = a ^ b ^ c;
          c = (a&b) | (c&(a^b)); }
        { vec_t a=n26, b=VONES;
          n59 = a ^ b ^ c;
          c = (a&b) | (c&(a^b)); }
        { vec_t a=n27, b=VONES;
          n60 = a ^ b ^ c;
          c = (a&b) | (c&(a^b)); }
    }
    n34 = n32 & n33;
    n36 = n32 & n35;
    n17 = n36 & n37;
    n38 = n4 & n16;
    n48 = n34 | n38;   /* reduce_bool (!=0) */
    n54 = n17 | n38;   /* reduce_bool (!=0) */
    n55 = ~((n15 ^ VONES) | (n17 ^ VZERO));   /* eq */
    n56 = ~((n15 ^ VZERO) | (n17 ^ VONES));   /* eq */
    n61 = (VZERO & ~n38) | (n11 & n38);   /* mux */
    n62 = (VZERO & ~n38) | (n12 & n38);   /* mux */
    n63 = (VZERO & ~n38) | (n13 & n38);   /* mux */
    n64 = (VZERO & ~n38) | (n14 & n38);   /* mux */
    n44 = (n61 & ~n34) | (n65 & n34);   /* mux */
    n45 = (n62 & ~n34) | (n66 & n34);   /* mux */
    n46 = (n63 & ~n34) | (n67 & n34);   /* mux */
    n47 = (n64 & ~n34) | (n68 & n34);   /* mux */
    n69 = (VZERO & ~n17) | (VZERO & n17);   /* mux */
    n53 = (n69 & ~n38) | (VONES & n38);   /* mux */
    n43 = n55 | n56;   /* reduce_bool (!=0) */
    {  /* pmux: 2 路 one-hot */
        n39 = (n28 & n55) | (n57 & n56);
        n40 = (n29 & n55) | (n58 & n56);
        n41 = (n30 & n55) | (n59 & n56);
        n42 = (n31 & n55) | (n60 & n56);
    }

    /* --- FF 更新（同步 reset + enable） --- */
    s->q24 = (n39 & (n43 & ~n3)) | (p->q24 & ~(n43 & ~n3) & ~n3);
    s->q25 = (n40 & (n43 & ~n3)) | (p->q25 & ~(n43 & ~n3) & ~n3);
    s->q26 = (n41 & (n43 & ~n3)) | (p->q26 & ~(n43 & ~n3) & ~n3);
    s->q27 = (n42 & (n43 & ~n3)) | (p->q27 & ~(n43 & ~n3) & ~n3);
    s->q49 = (n44 & (n48 & ~n3)) | (p->q49 & ~(n48 & ~n3) & ~n3);
    s->q50 = (n45 & (n48 & ~n3)) | (p->q50 & ~(n48 & ~n3) & ~n3);
    s->q51 = (n46 & (n48 & ~n3)) | (p->q51 & ~(n48 & ~n3) & ~n3);
    s->q52 = (n47 & (n48 & ~n3)) | (p->q52 & ~(n48 & ~n3) & ~n3);
    s->q18 = (n5 & (n38 & ~n3)) | (p->q18 & ~(n38 & ~n3) & ~n3);
    s->q19 = (n6 & (n38 & ~n3)) | (p->q19 & ~(n38 & ~n3) & ~n3);
    s->q20 = (n7 & (n38 & ~n3)) | (p->q20 & ~(n38 & ~n3) & ~n3);
    s->q21 = (n8 & (n38 & ~n3)) | (p->q21 & ~(n38 & ~n3) & ~n3);
    s->q22 = (n9 & (n38 & ~n3)) | (p->q22 & ~(n38 & ~n3) & ~n3);
    s->q23 = (n10 & (n38 & ~n3)) | (p->q23 & ~(n38 & ~n3) & ~n3);
    s->q32 = (n53 & (n54 & ~n3)) | (p->q32 & ~(n54 & ~n3) & ~n3);

    /* --- 輸出接腳 --- */
    *in_ready  = n16;
    *out_valid = n17;
    out_tag[0] = n18;
    out_tag[1] = n19;
    out_tag[2] = n20;
    out_tag[3] = n21;
    out_tag[4] = n22;
    out_tag[5] = n23;
}
#include <stdio.h>
#include <time.h>
int main(void){
    state_t a={0}, b={0}; vec_t rdy,ov,tag[6],it[6],il[4];
    volatile vec_t sink=0;
    vec_t lfsr = 0x123456789abcdefULL;
    const long N = 20000000;
    struct timespec t0,t1; clock_gettime(CLOCK_MONOTONIC,&t0);
    for(long i=0;i<N;i++){
        lfsr = (lfsr<<1) ^ (-(vec_t)(lfsr>>63) & 0x1B);   /* 每 cycle 換輸入 */
        for(int k=0;k<6;k++) it[k]=lfsr*(k+1);
        for(int k=0;k<4;k++) il[k]=lfsr>>(k+3);
        eval_cycle(&b,&a,VZERO,lfsr,it,il,lfsr>>7,&rdy,&ov,tag);
        a=b; sink ^= rdy ^ ov ^ tag[0];                   /* 消費輸出 */
    }
    clock_gettime(CLOCK_MONOTONIC,&t1);
    double s=(t1.tv_sec-t0.tv_sec)+(t1.tv_nsec-t0.tv_nsec)/1e9;
    printf("%ld target-cycles in %.3f s   (sink %llx)\n", N, s,(unsigned long long)sink);
    printf("  單 lane-group : %.1f M target-cycles/s\n", N/s/1e6);
    printf("  64 lanes      : %.0f M instance-cycles/s (單核)\n", N*64.0/s/1e6);
    return 0;
}
