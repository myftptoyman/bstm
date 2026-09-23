/* ipc_wp — 把真的 ooo_top 接到 lib/runtime 的 refill（含 wrong-path 游標狀態機），
 * 跑真 trace 量 IPC。Verilator 是純量模擬器，所以只開 lane 0。
 *
 *   用法： ipc_wp <trace_base> [cycles] [rob] [lsq] [iq]
 *   環境： BSTM_NOWP=1  收到 redirect 但不跳 shadow ——「零誤預測代價」的對照組。
 *
 * 建置見 test/tools/build_ipc.sh。ROB/LSQ/IQ 走 runtime cfg 埠，PRF 是 compile-time
 * 的 `PRF_N（沒有 cfg_prf_entries 埠），所以掃 PRF 必須重建。 */
#include "Vooo_top.h"
#include <cstdio>
#include <cstdint>
#include <cstdlib>
extern "C" {
#include "bstf_reader.h"
#include "refill.h"
}
static uint32_t g32(const vec_t*a,int n){ uint32_t v=0; for(int i=0;i<n;i++) if(a[i]&1) v|=1u<<i; return v; }
int main(int argc,char**argv){
    const char*base = argc>1?argv[1]:"/home/toyman/work/bstm/traces/coremark_1m";
    long maxc = argc>2?atol(argv[2]):500000;
    int c_rob=argc>3?atoi(argv[3]):64, c_lsq=argc>4?atoi(argv[4]):16, c_iq=argc>5?atoi(argv[5]):32;
    bstf_trace_t tr;
    if (bstf_open(&tr, base) < 0) { fprintf(stderr,"open %s failed\n",base); return 1; }
    printf("trace %s: %llu recs, shadow %llu B\n",base,
           (unsigned long long)tr.hdr->n_records,(unsigned long long)tr.hdr->shadow_bytes);

    Vooo_top*d=new Vooo_top;
    d->cfg_rob_entries=c_rob; d->cfg_iq_entries=c_iq;
    d->cfg_ldq_entries=c_lsq; d->cfg_stq_entries=c_lsq; d->cfg_mshr_entries=8;
    d->cfg_fetch_width=4; d->cfg_issue_width=4; d->cfg_commit_width=4;

    static bstm_refill_t rf; static bstm_fbwin_t win;
    static const bstf_trace_t *tps[BSTM_LANES];
    for (int i=0;i<BSTM_LANES;i++) tps[i]=&tr;
    bstm_refill_init(&rf,tps,1ull);          /* Verilator 是純量：只開 lane 0 */

    d->rst=1; d->fb_valid=0; for(int i=0;i<4;i++) d->fb_duop[i]=0;
    d->fb_fe_event=0; d->fb_mem_event=0;
    for(int i=0;i<4;i++){ d->clk=0;d->eval(); d->clk=1;d->eval(); }
    d->rst=0;

    const char *e_nowp = getenv("BSTM_NOWP");
    const int nowp = e_nowp && *e_nowp && *e_nowp != '0';   /* 空字串不算開啟 */
    long cy=0;
    while(cy<maxc && !(bstm_refill_done_mask(&rf)&1ull)){
        bstm_refill_window(&rf,&win);
        d->fb_valid = g32(win.valid,4);
        for(int i=0;i<4;i++) d->fb_duop[i] = g32(win.duop+i*32,32);
        d->fb_fe_event  = (uint8_t)g32(win.fe,8);
        d->fb_mem_event = g32(win.mem,32);
        d->clk=0; d->eval();
        vec_t take[3]; uint32_t t=d->fb_take;
        for(int i=0;i<3;i++) take[i] = (t>>i&1)?VONES:VZERO;
        vec_t rd = d->fb_redirect?VONES:VZERO;
        vec_t rs = (d->fb_redir_shadow && !nowp)?VONES:VZERO;
        d->clk=1; d->eval();
        bstm_refill_consume(&rf,take,rd,rs);
        cy++;
    }
    uint64_t cyc=d->cnt_cycles, ret=d->cnt_retired;
    printf("\n=== 真模型 + wrong-path ===\n");
    printf("  cycles %llu  retired %llu\n",(unsigned long long)cyc,(unsigned long long)ret);
    if(cyc) printf("  ** IPC = %.3f **\n",(double)ret/cyc);
    printf("\n  推測執行:\n");
    printf("    cnt_mispred    %llu\n",(unsigned long long)d->cnt_mispred);
    printf("    cnt_wrongpath  %llu\n",(unsigned long long)d->cnt_wrongpath);
    printf("    cnt_st_refill  %llu\n",(unsigned long long)d->cnt_st_refill);
    printf("\n  stall (cycles):  fetch %llu  rename %llu  backpr %llu  iq %llu  rob %llu  lsq %llu\n",
      (unsigned long long)d->cnt_st_fetch,(unsigned long long)d->cnt_st_rename,
      (unsigned long long)d->cnt_st_backpressure,(unsigned long long)d->cnt_st_iq,
      (unsigned long long)d->cnt_st_rob,(unsigned long long)d->cnt_st_lsq);
    printf("  lost (uop-slot): fetch %llu  refill %llu  rename %llu  iq %llu  lsq %llu\n",
      (unsigned long long)d->cnt_lost_fetch,(unsigned long long)d->cnt_lost_refill,
      (unsigned long long)d->cnt_lost_rename,(unsigned long long)d->cnt_lost_iq,
      (unsigned long long)d->cnt_lost_lsq);
    if(cyc) printf("  平均 ROB 佔用 %.2f\n",(double)d->cnt_rob_occ_sum/cyc);
    const bstm_lane_t*L=&rf.lane[0];
    printf("\n  runtime lane0: cp=%llu wp=%llu  enter=%llu leave=%llu starve=%llu\n",
      (unsigned long long)L->cp_consumed,(unsigned long long)L->wp_consumed,
      (unsigned long long)L->n_enter,(unsigned long long)L->n_leave,(unsigned long long)L->n_starve);
    printf("  blk: seen=%llu skipped=%llu redir_seen=%llu  nobranch=%llu resolved=%llu\n",
      (unsigned long long)L->n_blk_seen,(unsigned long long)L->n_blk_skipped,
      (unsigned long long)L->n_redir_blk_seen,(unsigned long long)L->n_nobranch,
      (unsigned long long)L->n_resolved);
    char msg[256];
    int rc=bstm_refill_check(&rf,0,(bstm_refill_done_mask(&rf)&1ull)?1:0,msg,sizeof msg);
    printf("  不變式檢查: %s\n", rc==0?"PASS":msg);
    return 0;
}
