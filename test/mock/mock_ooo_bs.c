#include "mock_ooo_bs.h"
#include "transpose.h"
#include "stats.h"
#include <stdlib.h>
#include <string.h>

/* ---------- bit-sliced 基本運算（模仿 bstm-cc 會產生的樣子） ---------- */
#define MUX(a,b,s)  (((a) & ~(s)) | ((b) & (s)))

/* out = a + b（n bit，wrap），cin 進位輸入 */
static void bs_add(vec_t *out, const vec_t *a, const vec_t *b, int n, vec_t cin)
{
    vec_t c = cin;
    int i;
    for (i = 0; i < n; i++) {
        vec_t x = a[i], y = b[i];
        out[i] = x ^ y ^ c;
        c = (x & y) | (c & (x ^ y));
    }
}
/* a < b（無號，n bit） */
static vec_t bs_ult(const vec_t *a, const vec_t *b, int n)
{
    vec_t lt = VZERO;
    int i;
    for (i = 0; i < n; i++) {
        vec_t x = a[i], y = b[i];
        lt = (~x & y) | (~(x ^ y) & lt);
    }
    return lt;
}
static vec_t bs_orn(const vec_t *a, int n)
{
    vec_t o = VZERO; int i;
    for (i = 0; i < n; i++) o |= a[i];
    return o;
}
/* c += inc（c 是 48 bit，inc 是 n bit 無號） */
static void bs_cnt_add(vec_t *c, const vec_t *inc, int n, vec_t en)
{
    vec_t carry = VZERO;
    int b;
    for (b = 0; b < BSTM_CNT_W; b++) {
        vec_t x = c[b], y = (b < n) ? (inc[b] & en) : VZERO;
        vec_t s = x ^ y ^ carry;
        carry = (x & y) | (carry & (x ^ y));
        c[b] = s;
    }
}

const char *const mock_counter_names[MOCK_NCNT] = {
    "cnt_cycles", "cnt_retired", "cnt_wrongpath", "cnt_st_fetch",
    "cnt_st_rename", "cnt_st_iq", "cnt_st_rob", "cnt_st_lsq",
    "cnt_st_mshr", "cnt_mispred", "cnt_rob_occ_sum"
};
enum { C_CYCLES, C_RETIRED, C_WRONGPATH, C_ST_FETCH, C_ST_RENAME,
       C_ST_IQ, C_ST_ROB, C_ST_LSQ, C_ST_MSHR, C_MISPRED, C_ROB_OCC };

/* ---------- eval：逐行對應 mock_ooo_top.v ---------- */
void mock_eval_rst(mock_state_t *s, const mock_state_t *p,
                   const bstm_fbwin_t *win, bstm_out_t *out, vec_t rst)
{
    const vec_t *cfg_rob = p->cfg[BSTM_CFG_ROB];
    const vec_t *cfg_iq  = p->cfg[BSTM_CFG_IQ];
    const vec_t *cfg_fw  = p->cfg[BSTM_CFG_FETCH_W];
    const vec_t *cfg_cw  = p->cfg[BSTM_CFG_COMMIT_W];
    vec_t en = p->enable;                /* rst 時所有暫存器歸零（下面 & ~rst） */
    vec_t nvalid[3], t3[3], want[3], take[3], ret[3], occ_t[7], occ_n[7];
    vec_t sum8[8], occ8[8], want8[8], robx[8];
    vec_t cwx[7], iqx[7], retx[7], nret[7], inc1[1], incv[1];
    vec_t fw_lt, rob_full, cw_gt, any_v, mispred, sh_end;
    vec_t st_fetch, st_rename, st_iq, st_rob, st_lsq, st_mshr;
    vec_t shc_n[6], shc_dec[6], one6[6], twelve[6], nz;
    int i, b;

    /* nvalid = v0+v1+v2+v3（3 bit） */
    nvalid[0] = nvalid[1] = nvalid[2] = VZERO;
    for (i = 0; i < BSTM_W; i++) {
        vec_t addend[3]; addend[0] = win->valid[i]; addend[1] = addend[2] = VZERO;
        bs_add(t3, nvalid, addend, 3, VZERO);
        nvalid[0] = t3[0]; nvalid[1] = t3[1]; nvalid[2] = t3[2];
    }

    fw_lt = bs_ult(nvalid, cfg_fw, 3);
    for (i = 0; i < 3; i++) want[i] = MUX(cfg_fw[i], nvalid[i], fw_lt);

    /* sum = {1'b0,occ} + {5'b0,want} */
    for (i = 0; i < 7; i++) occ8[i] = p->occ[i];
    occ8[7] = VZERO;
    for (i = 0; i < 3; i++) want8[i] = want[i];
    for (i = 3; i < 8; i++) want8[i] = VZERO;
    bs_add(sum8, occ8, want8, 8, VZERO);

    for (i = 0; i < 7; i++) robx[i] = cfg_rob[i];
    robx[7] = VZERO;
    rob_full = bs_ult(robx, sum8, 8);                 /* sum > cfg_rob */

    for (i = 0; i < 3; i++) take[i] = want[i] & ~rob_full;

    for (i = 0; i < 3; i++) cwx[i] = cfg_cw[i];
    for (i = 3; i < 7; i++) cwx[i] = VZERO;
    cw_gt = bs_ult(p->occ, cwx, 7);                   /* occ < cfg_commit_width */
    for (i = 0; i < 3; i++) ret[i] = MUX(cfg_cw[i], p->occ[i], cw_gt);

    any_v   = bs_orn(win->valid, BSTM_W);
    mispred = any_v & win->fe[7] & ~win->fe[4] & ~p->in_shadow;
    sh_end  = p->in_shadow & ~bs_orn(p->shadow_cnt, 6);

    out->take[0] = take[0]; out->take[1] = take[1]; out->take[2] = take[2];
    out->redirect     = mispred | sh_end;
    out->redir_shadow = mispred;

    /* stall 分類 */
    st_fetch  = ~bs_orn(nvalid, 3);
    st_rename = rob_full & bs_orn(want, 3);
    for (i = 0; i < 6; i++) iqx[i] = cfg_iq[i];
    iqx[6] = VZERO;
    st_iq     = ~bs_ult(p->occ, iqx, 7);              /* occ >= cfg_iq */
    st_rob    = rob_full;
    st_lsq    = bs_orn(win->mem, 8);
    st_mshr   = win->fe[3];

    /* occ' = occ + take - ret（7 bit wrap） */
    for (i = 0; i < 3; i++) retx[i] = ret[i];
    for (i = 3; i < 7; i++) retx[i] = VZERO;
    {
        vec_t takex[7];
        for (i = 0; i < 3; i++) takex[i] = take[i];
        for (i = 3; i < 7; i++) takex[i] = VZERO;
        bs_add(occ_t, p->occ, takex, 7, VZERO);
    }
    for (i = 0; i < 7; i++) nret[i] = ~retx[i];
    bs_add(occ_n, occ_t, nret, 7, VONES);             /* +(-ret) */

    /* shadow */
    nz = bs_orn(p->shadow_cnt, 6);
    for (i = 0; i < 6; i++) one6[i] = (i == 0) ? VONES : VZERO;
    for (i = 0; i < 6; i++) twelve[i] = ((12 >> i) & 1) ? VONES : VZERO;
    {
        vec_t none6[6];
        for (i = 0; i < 6; i++) none6[i] = ~one6[i];
        bs_add(shc_dec, p->shadow_cnt, none6, 6, VONES);   /* -1 */
    }
    for (i = 0; i < 6; i++) {
        vec_t v = MUX(VZERO, shc_dec[i], nz);
        shc_n[i] = MUX(v, twelve[i], mispred);
    }

    /* ---- 暫存器更新（帶 enable / rst） ---- */
    for (i = 0; i < 7; i++)
        s->occ[i] = ((occ_n[i] & ~rst) & en) | (p->occ[i] & ~en);
    {
        vec_t nsh = (mispred | (~mispred & ~sh_end & p->in_shadow)) & ~rst;
        s->in_shadow = (nsh & en) | (p->in_shadow & ~en);
    }
    for (i = 0; i < 6; i++)
        s->shadow_cnt[i] = ((shc_n[i] & ~rst) & en) | (p->shadow_cnt[i] & ~en);

    /* counters（rst 時歸零；enable=0 時凍結） */
    memcpy(s->cnt, p->cnt, sizeof s->cnt);
    inc1[0] = VONES;
    bs_cnt_add(s->cnt[C_CYCLES],    inc1, 1, en);
    bs_cnt_add(s->cnt[C_RETIRED],   ret,  3, en);
    { vec_t wp[3]; for (i = 0; i < 3; i++) wp[i] = take[i] & p->in_shadow;
      bs_cnt_add(s->cnt[C_WRONGPATH], wp, 3, en); }
    incv[0] = st_fetch;  bs_cnt_add(s->cnt[C_ST_FETCH],  incv, 1, en);
    incv[0] = st_rename; bs_cnt_add(s->cnt[C_ST_RENAME], incv, 1, en);
    incv[0] = st_iq;     bs_cnt_add(s->cnt[C_ST_IQ],     incv, 1, en);
    incv[0] = st_rob;    bs_cnt_add(s->cnt[C_ST_ROB],    incv, 1, en);
    incv[0] = st_lsq;    bs_cnt_add(s->cnt[C_ST_LSQ],    incv, 1, en);
    incv[0] = st_mshr;   bs_cnt_add(s->cnt[C_ST_MSHR],   incv, 1, en);
    incv[0] = mispred;   bs_cnt_add(s->cnt[C_MISPRED],   incv, 1, en);
    bs_cnt_add(s->cnt[C_ROB_OCC], p->occ, 7, en);
    if (rst) for (i = 0; i < MOCK_NCNT; i++)
        for (b = 0; b < BSTM_CNT_W; b++) s->cnt[i][b] &= ~rst;

    memcpy(s->cfg, p->cfg, sizeof s->cfg);
    s->enable = p->enable;
}

/* ---------- bstm_model_t 綁定 ---------- */
static const bstm_model_desc_t g_desc = {
    "mock_ooo_top", BSTM_LANES, (uint32_t)sizeof(mock_state_t), MOCK_NCNT,
    mock_counter_names, BSTM_CNT_W, BSTM_CFG_N, bstm_cfg_names, bstm_cfg_widths
};
static const bstm_model_desc_t *m_describe(void) { return &g_desc; }
static void *m_alloc(void) { return calloc(1, sizeof(mock_state_t)); }
static void  m_free(void *s) { free(s); }

static void m_init(void *sv, const uint32_t *cfg, uint64_t lane_mask)
{
    mock_state_t *s = sv;
    int f, l;
    memset(s, 0, sizeof *s);
    s->enable = lane_mask;
    for (f = 0; f < BSTM_CFG_N; f++) {
        uint8_t v[BSTM_LANES];
        for (l = 0; l < BSTM_LANES; l++)
            v[l] = (uint8_t)(((lane_mask >> l) & 1) ? cfg[l * BSTM_CFG_N + f] : 0);
        bstm_pack_lanes_small(v, s->cfg[f], 8);
    }
}
static void m_eval(void *n, const void *c, const bstm_fbwin_t *w, bstm_out_t *o)
{ mock_eval_rst(n, c, w, o, VZERO); }
static void m_extract(const void *sv, uint64_t *out)
{
    const mock_state_t *s = sv;
    bstm_stats_extract(&s->cnt[0][0], MOCK_NCNT, BSTM_CNT_W, out);
}
static uint64_t m_done(const void *sv) { (void)sv; return 0; }
static void m_enable(void *sv, uint64_t mask) { ((mock_state_t *)sv)->enable = mask; }

const bstm_model_t bstm_mock_model = {
    m_describe, m_alloc, m_free, m_init, m_eval, m_extract, m_done, m_enable
};
