/* mkbstf — 產生合成 .bstf / .fe / .mem / .fe.wp / .meta.json
 * 給 runtime 單元測試與對拍用（真 trace 由 Agent A/B 產生）。
 *   mkbstf -o traces/synth0 -n 20000 -d 32 -s 1 --simpoint 0
 * 版面：hdr | N 筆 correct-path | K*D 筆 shadow
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bstf.h"

/* 與 bstf_rec_has_mem() 同一條規則：只看 uop_class */
#define MEMOP(uc) ((uc)==UC_LOAD || (uc)==UC_STORE || (uc)==UC_AMO)

static uint64_t rs = 12345;
static uint32_t rnd(void){ rs ^= rs<<13; rs ^= rs>>7; rs ^= rs<<17; return (uint32_t)(rs>>16); }

int main(int argc, char **argv)
{
    const char *out = "synth";
    long N = 20000; int D = 32; unsigned simpoint = 0; uint32_t seed = 1;
    int i;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i+1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "-n") && i+1 < argc) N = atol(argv[++i]);
        else if (!strcmp(argv[i], "-d") && i+1 < argc) D = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-s") && i+1 < argc) seed = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--simpoint") && i+1 < argc) simpoint = (unsigned)atoi(argv[++i]);
        else { fprintf(stderr, "usage: %s -o base -n N -d D -s seed --simpoint id\n", argv[0]); return 2; }
    }
    rs = 0x9E3779B97F4A7C15ULL ^ seed;

    bstf_rec_t *rec = calloc((size_t)N, sizeof *rec);
    unsigned char *isbr = calloc((size_t)N, 1);
    long nblk = 0, nmem = 0, nbr = 0;
    int in_blk = 0;

    /* ---- 先決定每一筆的性質 ---- */
    for (i = 0; i < N; i++) {
        uint32_t r = rnd() % 100;
        bstf_rec_t *R = &rec[i];
        if (r < 45)       R->uop_class = UC_ALU,   R->exec_lat = 1;
        else if (r < 60)  R->uop_class = UC_LOAD,  R->exec_lat = 2, R->mem_size = 3;
        else if (r < 70)  R->uop_class = UC_STORE, R->exec_lat = 1, R->mem_size = 3, R->mem_store = 1;
        else if (r < 72)  R->uop_class = UC_AMO,   R->exec_lat = 4, R->mem_size = 3, R->mem_store = 1;
        else if (r < 80)  R->uop_class = UC_MUL,   R->exec_lat = 3;
        else if (r < 95)  R->uop_class = UC_BRANCH,R->exec_lat = 1;
        else              R->uop_class = UC_JALR,  R->exec_lat = 1, R->flags |= BF_CALL;

        R->src1 = (uint8_t)(0x80 | (rnd() & 0x1F));
        R->src2 = (uint8_t)((rnd() & 1) ? (0x80 | (rnd() & 0x1F)) : 0x3F);
        R->dst  = (uint8_t)((R->uop_class == UC_STORE) ? 0x3F : (0x80 | (rnd() & 0x1F)));

        in_blk++;
        if (R->uop_class == UC_BRANCH || R->uop_class == UC_JALR || in_blk >= 6) {
            R->is_block_end = 1; R->flags |= BF_BLK_END; in_blk = 0;
        }
        if (R->uop_class == UC_BRANCH || R->uop_class == UC_JALR) {
            if ((rnd() % 100) < 30) { isbr[i] = 1; nbr++; }   /* 30% 會誤預測 -> 有 shadow */
        }
    }
    rec[N-1].flags |= BF_INTERVAL_END | BF_BLK_END;
    rec[N-1].is_block_end = 1;

    /* ---- fe / mem 的 delta 與計數 ----
     * 依 CONTRACT §5：idx(i) = sum_{j<=i} delta(j) - delta(0)。
     * 照 Agent H 的實際產生方式：
     *   .fe  每個 block 的第一條記錄 delta=1（含第 0 筆）-> idx(0)=1-1=0
     *   .mem 每筆存取 delta=1，但第一筆存取 delta=0（它就是索引 0）
     * 讀取端只要「不加 delta(0)」兩者都會落在 [0, n-1]。 */
    {
        int prev_blk_end = 1, first_mem = 1;
        for (i = 0; i < N; i++) {
            bstf_rec_t *R = &rec[i];
            int is_mem = MEMOP(R->uop_class);
            if (prev_blk_end) { R->fe_index_delta = 1; nblk++; }
            else               R->fe_index_delta = 0;
            prev_blk_end = R->is_block_end;

            if (is_mem) {
                R->mem_index_delta = first_mem ? 0 : 1;
                first_mem = 0; nmem++;
            } else R->mem_index_delta = 0;
        }
        if (nmem == 0) nmem = 1;
    }

    /* ---- shadow 區 ---- */
    long nsh = nbr * D;
    bstf_rec_t *sh = calloc((size_t)(nsh ? nsh : 1), sizeof *sh);
    uint64_t sh_off0 = sizeof(bstf_hdr_t) + (uint64_t)N * sizeof(bstf_rec_t);
    {
        long k = 0;
        for (i = 0; i < N; i++) {
            if (!isbr[i]) continue;
            rec[i].shadow_off = (uint32_t)(sh_off0 + (uint64_t)k * D * sizeof(bstf_rec_t));
            rec[i].shadow_len = (uint16_t)D;
            for (int m = 0; m < D; m++) {
                bstf_rec_t *S = &sh[k*D + m];
                uint32_t r = rnd() % 100;
                S->uop_class = (r < 55) ? UC_ALU : (r < 75) ? UC_LOAD : UC_ALU;   /* shadow 的 mem 事件走 .mem.wp */
                S->exec_lat  = 1;
                S->src1 = (uint8_t)(0x80 | (rnd() & 0x1F));
                S->src2 = 0x3F;
                S->dst  = (uint8_t)(0x80 | (rnd() & 0x1F));
                if ((m % 5) == 4) { S->is_block_end = 1; S->flags |= BF_BLK_END; }
                S->fe_index_delta = 0;      /* shadow 期間 .fe 游標凍結（v2 A1） */
                S->mem_index_delta = 0;
            }
            k++;
        }
    }

    /* ---- 寫檔 ---- */
    char p[600]; FILE *f;
    bstf_hdr_t h; memset(&h, 0, sizeof h);
    h.magic = BSTF_MAGIC; h.version = BSTF_VERSION;
    h.rec_bytes = (uint32_t)sizeof(bstf_rec_t);
    h.n_records = (uint64_t)N;
    h.shadow_bytes  = (uint64_t)nsh * sizeof(bstf_rec_t);
    h.shadow_offset = sh_off0;
    h.n_fe_blocks   = (uint64_t)nblk;
    h.n_mem_access  = (uint64_t)nmem;
    h.simpoint_id = simpoint; h.simpoint_weight = 1 << 16;
    snprintf(h.workload, sizeof h.workload, "synth");
    snprintf(h.isa, sizeof h.isa, "rv64gc");

    snprintf(p, sizeof p, "%s.bstf", out); f = fopen(p, "wb");
    if (!f) { perror(p); return 1; }
    fwrite(&h, sizeof h, 1, f);
    fwrite(rec, sizeof *rec, (size_t)N, f);
    if (nsh) fwrite(sh, sizeof *sh, (size_t)nsh, f);
    fclose(f);

    snprintf(p, sizeof p, "%s.fe", out); f = fopen(p, "wb");
    for (i = 0; i < nblk; i++) {
        unsigned char b = (unsigned char)(rnd() & 0x3);             /* bubbles */
        if (rnd() & 1) b |= FE_UBTB_HIT;
        if ((rnd() % 100) < 92) b |= FE_DIR_OK;                     /* 8% 方向錯 */
        if (rnd() & 1) b |= FE_TGT_OK;
        if ((rnd() % 100) < 25) b |= FE_REDIRECT;
        fwrite(&b, 1, 1, f);
    }
    fclose(f);

    snprintf(p, sizeof p, "%s.mem", out); f = fopen(p, "wb");
    for (i = 0; i < nmem; i++) {
        uint32_t r = rnd() % 100;
        unsigned char b = (r < 85) ? 0 : (r < 95) ? 1 : (r < 99) ? 2 : 3;   /* level */
        unsigned char cls = (b == 0) ? 0 : (b == 1) ? 2 : (b == 2) ? 4 : 6;
        b |= (unsigned char)(cls << 4);
        if ((rnd() % 100) < 3) b |= MEM_TLB_MISS;
        fwrite(&b, 1, 1, f);
    }
    fclose(f);

    snprintf(p, sizeof p, "%s.fe.wp", out); f = fopen(p, "wb");
    for (i = 0; i < nsh; i++) {
        unsigned char b = (unsigned char)((rnd() & 0x3) | FE_WRONGPATH);
        fwrite(&b, 1, 1, f);
    }
    fclose(f);

    snprintf(p, sizeof p, "%s.mem.wp", out); f = fopen(p, "wb");
    for (i = 0; i < nsh; i++) { unsigned char b = (unsigned char)(rnd() & 0x33); fwrite(&b,1,1,f); }
    fclose(f);

    snprintf(p, sizeof p, "%s.meta.json", out); f = fopen(p, "wb");
    fprintf(f, "{\n  \"generator\": \"mkbstf\",\n  \"seed\": %u,\n"
               "  \"wrongpath_depth\": %d,\n  \"simpoint_id\": %u,\n"
               "  \"cache_cfg\": \"L1-32k-8w/L2-512k-8w/L3-8M-16w\"\n}\n",
            seed, D, simpoint);
    fclose(f);

    printf("%s: %ld rec, %ld blk, %ld mem, %ld shadow-rec (%ld branch x D=%d)\n",
           out, N, nblk, nmem, nsh, nbr, D);
    free(rec); free(sh); free(isbr);
    return 0;
}
