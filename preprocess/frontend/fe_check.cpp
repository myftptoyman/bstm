/* ============================================================
 * fe_check.cpp — 檢查 / 傾印 .fe overlay（BSTM Agent A 自測用）
 *   fe_check <file.fe> [--dump N]
 * 用 include/bstf.h 的 FE_* 巨集解碼，確認欄位合法並印出統計。
 * 回傳 0 = 檢查通過。
 * ============================================================ */
#include "../../include/bstf.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <file.fe> [--dump N]\n", argv[0]); return 2; }
    long dump = 0;
    for (int i = 2; i < argc; i++)
        if (!strcmp(argv[i], "--dump") && i + 1 < argc) dump = strtol(argv[i + 1], nullptr, 0);

    FILE* f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "fe_check: cannot open %s\n", argv[1]); return 2; }

    unsigned long long n = 0, bub[4] = {0, 0, 0, 0};
    unsigned long long ubtb = 0, ovr = 0, dir_ok = 0, tgt_ok = 0, wp = 0, redir = 0;
    unsigned long long wp_redir = 0, cp = 0, cp_bub = 0;
    int bad = 0;
    unsigned char buf[65536];
    size_t got;
    while ((got = fread(buf, 1, sizeof(buf), f)) > 0) {
        for (size_t i = 0; i < got; i++) {
            unsigned char b = buf[i];
            unsigned bb = FE_BUBBLES(b);
            bub[bb]++;
            if (b & FE_UBTB_HIT)     ubtb++;
            if (b & FE_BTB_OVERRIDE) ovr++;
            if (b & FE_DIR_OK)       dir_ok++;
            if (b & FE_TGT_OK)       tgt_ok++;
            if (b & FE_REDIRECT)     redir++;
            if (b & FE_WRONGPATH) {
                wp++;
                if (b & FE_REDIRECT) wp_redir++;
            } else {
                cp++;
                cp_bub += bb;
            }
            /* 合法性：redirect 必然伴隨 dir 或 tgt 預測錯誤 */
            if ((b & FE_REDIRECT) && (b & FE_DIR_OK) && (b & FE_TGT_OK)) {
                if (bad < 8) fprintf(stderr, "fe_check: block %llu: REDIRECT 但 DIR_OK+TGT_OK 皆為 1 (0x%02x)\n", n, b);
                bad++;
            }
            /* bubble 只能是 0/1/2（本模型最多兩級 override） */
            if (bb > 2) {
                if (bad < 8) fprintf(stderr, "fe_check: block %llu: bubbles=%u > 2 (0x%02x)\n", n, bb, b);
                bad++;
            }
            /* BTB override 一定至少 1 bubble */
            if ((b & FE_BTB_OVERRIDE) && bb == 0) {
                if (bad < 8) fprintf(stderr, "fe_check: block %llu: BTB_OVERRIDE 但 bubbles=0 (0x%02x)\n", n, b);
                bad++;
            }
            if ((long)n < dump)
                printf("%8llu 0x%02x bub=%u ubtb=%d ovr=%d dir=%d tgt=%d wp=%d redir=%d\n",
                       n, b, bb, !!(b & FE_UBTB_HIT), !!(b & FE_BTB_OVERRIDE),
                       !!(b & FE_DIR_OK), !!(b & FE_TGT_OK), !!(b & FE_WRONGPATH),
                       !!(b & FE_REDIRECT));
            n++;
        }
    }
    fclose(f);

    printf("fe_check %s\n", argv[1]);
    printf("  blocks            : %llu (correct %llu / wrong-path %llu)\n", n, cp, wp);
    printf("  bubbles 0/1/2/3   : %llu / %llu / %llu / %llu\n", bub[0], bub[1], bub[2], bub[3]);
    printf("  uBTB hit          : %llu (%.2f%%)\n", ubtb, n ? 100.0 * ubtb / n : 0.0);
    printf("  BTB override      : %llu (%.2f%%)\n", ovr,  n ? 100.0 * ovr  / n : 0.0);
    printf("  DIR ok / TGT ok   : %llu / %llu\n", dir_ok, tgt_ok);
    printf("  redirect          : %llu (correct-path %llu)\n", redir, redir - wp_redir);
    printf("  avg bubbles (cp)  : %.4f\n", cp ? (double)cp_bub / (double)cp : 0.0);
    if (bad) { printf("  FAIL: %d 個不合法的 block\n", bad); return 1; }
    printf("  OK\n");
    return 0;
}
