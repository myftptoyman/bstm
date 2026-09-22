/* bstfdump — 檢查一份 .bstf + overlay 是否能被 runtime 正確讀
 *   bstfdump <base> [n_records_to_print] [--lax]
 * 給整合用：Agent A/B 產生 overlay 之後，先用這支確認格式對得上。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bstf_reader.h"

static const char *ucn[16] = {"ALU","MUL","DIV","FPU","LOAD","STORE","BRANCH","JUMP",
                              "JALR","RET","CSR","FENCE","AMO","NOP","SYS","VEC"};

int main(int argc, char **argv)
{
    const char *base = argc > 1 ? argv[1] : "traces/synth0";
    long nprint = argc > 2 ? atol(argv[2]) : 8;
    int lax = 0, i;
    bstf_trace_t t; bstf_cursor_t c;
    char buf[256];
    for (i = 1; i < argc; i++) if (!strcmp(argv[i], "--lax")) lax = 1;

    if (bstf_open_ex(&t, base, lax ? BSTF_LAX : BSTF_STRICT)) {
        fprintf(stderr, "open %s: %s\n", base, bstf_strerror());
        return 1;
    }
    printf("== %s ==\n", base);
    printf("  version %u  rec_bytes %u  hdr_bytes %zu  sizeof(bstf_rec_t)=%zu\n",
           t.hdr->version, t.rec_bytes, sizeof(bstf_hdr_t), sizeof(bstf_rec_t));
    printf("  workload=%.64s isa=%.32s simpoint=%u weight=%u icount_start=%llu\n",
           t.hdr->workload, t.hdr->isa, t.hdr->simpoint_id, t.hdr->simpoint_weight,
           (unsigned long long)t.hdr->icount_start);
    printf("  n_records=%llu  shadow: off=%llu bytes=%llu (%llu rec)\n",
           (unsigned long long)t.n_records,
           (unsigned long long)t.hdr->shadow_offset,
           (unsigned long long)t.hdr->shadow_bytes,
           (unsigned long long)(t.hdr->shadow_bytes / t.rec_bytes));
    printf("  overlay: .fe %llu/%llu  .mem %llu/%llu  .fe.wp %llu  .mem.wp %llu  .imem %llu\n",
           (unsigned long long)t.n_fe,  (unsigned long long)t.hdr->n_fe_blocks,
           (unsigned long long)t.n_mem, (unsigned long long)t.hdr->n_mem_access,
           (unsigned long long)t.n_fe_wp, (unsigned long long)t.n_mem_wp,
           (unsigned long long)t.n_imem);
    printf("  wrong-path 排版：.fe 裡 FE_REDIRECT 的 block 數 N=%llu，"
           ".fe.wp D=%llu block/次，shadow stride=%llu 記錄/次\n",
           (unsigned long long)t.n_mispred, (unsigned long long)t.fe_wp_depth,
           (unsigned long long)t.shadow_stride);
    if (t.meta && bstf_meta_get(&t, "config_fingerprint", buf, sizeof buf) == 0)
        printf("  meta config_fingerprint = %s\n", buf);

    /* 全檔掃描：驗證 CONTRACT §5 的 idx(i) = sum_{j<=i} delta(j) - delta(0) */
    {
        uint64_t fe = 0, mem = 0, nsh = 0, nmemop = 0, r;
        uint64_t bad_gap = 0;
        int64_t  prev_mem = -1;
        bstf_cur_init(&t, &c);
        printf("  起點：fe_idx=%llu mem_idx=%llu（兩者都必須是 0）\n",
               (unsigned long long)c.fe_idx, (unsigned long long)c.mem_idx);
        for (r = 0; r < t.n_records; r++) {
            const bstf_rec_t *R = bstf_peek(&t, &c);
            if (!R) { printf("  !! peek NULL at record %llu\n", (unsigned long long)r); break; }
            if (c.fe_idx > fe) fe = c.fe_idx;
            if (c.mem_idx > mem) mem = c.mem_idx;
            if (R->shadow_off) nsh++;
            if (bstf_rec_has_mem(R)) {
                /* 每一筆真正的存取都要剛好對到下一個 .mem entry */
                if ((int64_t)c.mem_idx != prev_mem + 1) bad_gap++;
                prev_mem = (int64_t)c.mem_idx;
                nmemop++;
            }
            bstf_advance(&t, &c, 1);
        }
        printf("  掃描完：max fe_idx=%llu (檔案 %llu)  max mem_idx=%llu (檔案 %llu)  帶 shadow 的記錄 %llu 筆\n",
               (unsigned long long)fe, (unsigned long long)t.hdr->n_fe_blocks,
               (unsigned long long)mem, (unsigned long long)t.hdr->n_mem_access,
               (unsigned long long)nsh);
        printf("  LOAD/STORE/AMO 記錄 %llu 筆 vs hdr.n_mem_access %llu%s\n",
               (unsigned long long)nmemop, (unsigned long long)t.hdr->n_mem_access,
               (t.hdr->n_mem_access && nmemop != t.hdr->n_mem_access) ? "   << 不一致!" : "  OK");
        if (bad_gap)
            printf("  !! 有 %llu 筆存取的 mem_idx 不是連續遞增 —— delta 與 uop_class 對不上\n",
                   (unsigned long long)bad_gap);
        if (t.hdr->n_fe_blocks && fe + 1 != t.hdr->n_fe_blocks)
            printf("  !! fe 游標終點 %llu != n_fe_blocks %llu —— delta 累加與 overlay 對不上\n",
                   (unsigned long long)(fe + 1), (unsigned long long)t.hdr->n_fe_blocks);
        if (t.hdr->n_mem_access && mem + 1 != t.hdr->n_mem_access)
            printf("  !! mem 游標終點 %llu != n_mem_access %llu\n",
                   (unsigned long long)(mem + 1), (unsigned long long)t.hdr->n_mem_access);
    }

    bstf_cur_init(&t, &c);
    for (i = 0; i < nprint; i++) {
        const bstf_rec_t *R = bstf_peek(&t, &c);
        if (!R) break;
        printf("  [%4d] %-6s lat=%-2u s1=%02x s2=%02x d=%02x flags=%02x mem(sz=%u st=%u) "
               "blk_end=%u fe=%llu(%02x) mem=%llu(%02x) shadow=%u/%u duop=%08x\n",
               i, ucn[R->uop_class], R->exec_lat, R->src1, R->src2, R->dst, R->flags,
               R->mem_size, R->mem_store, R->is_block_end,
               (unsigned long long)c.fe_idx,  bstf_fe_event(&t, &c),
               (unsigned long long)c.mem_idx, bstf_mem_event_of(&t, &c, R),
               R->shadow_off, R->shadow_len, bstf_rec_to_duop(R, 0));
        bstf_advance(&t, &c, 1);
    }
    bstf_close(&t);
    return 0;
}
