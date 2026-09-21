#include "tap.h"
#include "bstf_reader.h"
#include <string.h>
int main(int argc,char**argv){
    const char *base = argc>1?argv[1]:"traces/synth0";
    bstf_trace_t t; bstf_cursor_t c, c2;
    uint64_t i, fe=0, mem=0; char buf[128];
    if (bstf_open(&t, base)) { fprintf(stderr,"open %s: %s\n",base,bstf_strerror()); return 1; }

    CHECK(t.hdr->magic==BSTF_MAGIC,"magic");
    CHECK(t.n_fe==t.hdr->n_fe_blocks,"fe size %llu vs %llu",
          (unsigned long long)t.n_fe,(unsigned long long)t.hdr->n_fe_blocks);
    CHECK(t.n_mem==t.hdr->n_mem_access,"mem size");
    CHECK(t.fe_wp!=NULL,".fe.wp mapped");
    CHECK(t.meta!=NULL,".meta.json read");
    CHECK(bstf_meta_get(&t,"generator",buf,sizeof buf)==0 && !strcmp(buf,"mkbstf"),
          "meta generator=%s",buf);

    /* 順序掃描：CONTRACT §5 的 idx(i) = sum_{j<=i} delta(j) - delta(0)
     * 所以不管 delta(0) 是 0（.mem）還是 1（.fe），idx(0) 都必須是 0。 */
    bstf_cur_init(&t,&c);
    CHECK(c.fe_idx==0 && c.mem_idx==0,"idx(0) 必須是 0（delta(0) 不可加進去）");
    { const bstf_rec_t *r0 = bstf_peek(&t,&c);
      CHECK(r0 && r0->fe_index_delta==1,"mkbstf 照 H 的約定：rec[0].fe_index_delta==1"); }
    { uint64_t nmemop=0; int64_t prev=-1; long gap=0;
      for(i=0;i<t.n_records;i++){
        const bstf_rec_t *r = bstf_peek(&t,&c);
        if(!r){ CHECK(0,"peek null at %llu",(unsigned long long)i); break; }
        if(c.fe_idx>fe) fe=c.fe_idx;
        if(c.mem_idx>mem) mem=c.mem_idx;
        if(bstf_rec_has_mem(r)){
            /* delta 不能拿來判斷「有沒有存取」，但 uop_class 可以，
             * 而且每一筆存取的 mem_idx 必須連續遞增。 */
            if((int64_t)c.mem_idx != prev+1) gap++;
            prev=(int64_t)c.mem_idx; nmemop++;
            CHECK(bstf_mem_event_of(&t,&c,r)==bstf_mem_event(&t,&c),"mem 記錄兩種取法一致");
        } else {
            CHECK(bstf_mem_event_of(&t,&c,r)==0,"非 mem 記錄的 mem event 必須是 0");
        }
        bstf_advance(&t,&c,1);
      }
      CHECK(gap==0,"每筆 LOAD/STORE/AMO 的 mem_idx 連續遞增（%ld 筆不連續）",gap);
      CHECK(nmemop==t.hdr->n_mem_access,"LOAD/STORE/AMO 筆數 %llu == n_mem_access %llu",
            (unsigned long long)nmemop,(unsigned long long)t.hdr->n_mem_access);
    }
    CHECK(!bstf_avail(&t,&c),"EOF after n_records");
    CHECK(fe==t.hdr->n_fe_blocks-1,"max fe_idx %llu vs %llu",
          (unsigned long long)fe,(unsigned long long)t.hdr->n_fe_blocks-1);
    CHECK(mem==t.hdr->n_mem_access-1,"max mem_idx %llu",(unsigned long long)mem);

    /* seek_index 與順序推進一致 */
    bstf_cur_seek_index(&t,&c,1234);
    bstf_cur_init(&t,&c2); bstf_advance(&t,&c2,1234);
    CHECK(c.rec_byte==c2.rec_byte && c.fe_idx==c2.fe_idx && c.mem_idx==c2.mem_idx,
          "seek vs advance: %llu/%llu %llu/%llu",
          (unsigned long long)c.fe_idx,(unsigned long long)c2.fe_idx,
          (unsigned long long)c.mem_idx,(unsigned long long)c2.mem_idx);
    CHECK(bstf_cur_index(&t,&c)==1234,"cur_index");

    /* shadow：找第一個有 shadow 的記錄，進去走完再回來 */
    bstf_cur_init(&t,&c);
    for(i=0;i<t.n_records;i++){
        const bstf_rec_t *r = bstf_peek(&t,&c);
        if(r && r->shadow_off) break;
        bstf_advance(&t,&c,1);
    }
    CHECK(i<t.n_records,"found a branch with shadow");
    if(i<t.n_records){
        bstf_cursor_t save=c; uint32_t n; uint64_t wp0;
        const bstf_rec_t *br = bstf_peek(&t,&c);
        uint16_t len = br->shadow_len;
        CHECK(bstf_enter_shadow(&t,&c)==1,"enter_shadow");
        CHECK(c.in_shadow && c.shadow_left==len,"shadow_left=%u",c.shadow_left);
        wp0 = c.wp_idx;
        CHECK(bstf_fe_event(&t,&c) & FE_WRONGPATH,"shadow fe from .fe.wp");
        CHECK(c.fe_idx==save.fe_idx,"fe cursor frozen in shadow");
        n = bstf_advance(&t,&c,len);
        CHECK(n==len,"walked %u/%u shadow recs",n,len);
        CHECK(c.wp_idx==wp0+len,"wp_idx advanced");
        CHECK(!bstf_avail(&t,&c),"shadow exhausted");
        bstf_leave_shadow(&t,&c);
        { bstf_cursor_t exp=save; bstf_advance(&t,&exp,1);
          CHECK(c.rec_byte==exp.rec_byte && c.fe_idx==exp.fe_idx && c.mem_idx==exp.mem_idx,
                "resume point == branch+1"); }
    }

    /* duop 打包 */
    { bstf_rec_t r; memset(&r,0,sizeof r);
      r.uop_class=UC_LOAD; r.exec_lat=5; r.src1=0x80|7; r.src2=0x3F;
      r.dst=0x80|9; r.flags=BF_SERIALIZE|BF_BLK_END; r.mem_store=0;
      uint32_t d = bstf_rec_to_duop(&r,1);
      CHECK(bstf_rec_has_mem(&r),"UC_LOAD 算 mem op");
      { bstf_rec_t a=r; a.uop_class=UC_AMO;   CHECK(bstf_rec_has_mem(&a),"UC_AMO 算 mem op"); }
      { bstf_rec_t a=r; a.uop_class=UC_ALU;   CHECK(!bstf_rec_has_mem(&a),"UC_ALU 不算"); }
      { bstf_rec_t a=r; a.uop_class=UC_STORE; CHECK(bstf_rec_has_mem(&a),"UC_STORE 算"); }
      CHECK((d>>31)&1,"wrongpath");
      CHECK((d>>30)&1,"blkend");
      CHECK((d>>29)&1,"serialize");
      CHECK(((d>>24)&0xF)==UC_LOAD,"class");
      CHECK(((d>>20)&0xF)==5,"lat");
      CHECK(((d>>19)&1)==1 && ((d>>13)&0x3F)==7,"src1");
      CHECK(((d>>12)&1)==0,"src2 invalid");
      CHECK(((d>>5)&1)==1 && (d&0x1F)==9,"dst"); }

    bstf_close(&t);
    TAP_DONE("bstf_reader");
}
