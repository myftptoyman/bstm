// ============================================================
// bstf_check.cc -- validate a .bstf file and re-derive its statistics
// independently of bstf_gen (so a bug has to appear twice to slip through).
//
//   bstf_check <file.bstf> [--fe <file.frontend.txt>] [--mem <file.mem.txt>]
// ============================================================
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstddef>
#include <cstdint>
#include <cinttypes>
#include <string>
#include <vector>
#include <algorithm>
#include <utility>

extern "C" {
#include "bstf.h"
}

static const char* kClassName[16] = {
    "ALU","MUL","DIV","FPU","LOAD","STORE","BRANCH","JUMP",
    "JALR","RET","CSR","FENCE","AMO","NOP","SYS","VEC"
};

int main(int argc, char** argv)
{
    if (argc < 2) { fprintf(stderr, "usage: bstf_check <file.bstf> [--fe F] [--mem F] [--imem F] [--memwp F]\n"); return 2; }
    const char* path = argv[1];
    const char* fe_path = nullptr, *mem_path = nullptr, *imem_path = nullptr, *memwp_path = nullptr;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--fe"))   fe_path   = argv[++i];
        if (!strcmp(argv[i], "--mem"))  mem_path  = argv[++i];
        if (!strcmp(argv[i], "--imem")) imem_path = argv[++i];
        if (!strcmp(argv[i], "--memwp")) memwp_path = argv[++i];
    }

    FILE* f = fopen(path, "rb");
    if (!f) { perror(path); return 1; }
    bstf_hdr_t h;
    if (fread(&h, 1, sizeof h, f) != sizeof h) { fprintf(stderr, "short header\n"); return 1; }

    int errs = 0;
    printf("=== header: %s ===\n", path);
    printf("magic          : 0x%" PRIx64 " %s\n", h.magic, h.magic == BSTF_MAGIC ? "(BSTF1 ok)" : "(BAD)");
    if (h.magic != BSTF_MAGIC) errs++;
    printf("version        : %u\n", h.version);
    printf("rec_bytes      : %u   (sizeof(bstf_rec_t) = %zu)%s\n", h.rec_bytes, sizeof(bstf_rec_t),
           h.rec_bytes == sizeof(bstf_rec_t) ? "" : "   <-- MISMATCH");
    if (h.rec_bytes != sizeof(bstf_rec_t)) errs++;
    printf("hdr offsets    : workload@%zu isa@%zu  (sizeof(bstf_hdr_t) = %zu)\n",
           offsetof(bstf_hdr_t, workload), offsetof(bstf_hdr_t, isa), sizeof(bstf_hdr_t));
    printf("n_records      : %" PRIu64 "\n", h.n_records);
    printf("n_fe_blocks    : %" PRIu64 "\n", h.n_fe_blocks);
    printf("n_mem_access   : %" PRIu64 "\n", h.n_mem_access);
    printf("icount_start   : %" PRIu64 "\n", h.icount_start);
    printf("shadow         : off=%" PRIu64 " bytes=%" PRIu64 "\n", h.shadow_offset, h.shadow_bytes);
    printf("simpoint       : id=%u weight=%u (Q16 -> %.4f)\n",
           h.simpoint_id, h.simpoint_weight, h.simpoint_weight / 65536.0);
    printf("workload       : \"%.64s\"  (%zu chars)\n", h.workload, strnlen(h.workload, 64));
    printf("isa            : \"%.32s\"  (%zu chars)\n", h.isa, strnlen(h.isa, 32));
    if (!h.workload[0]) { printf("!! workload string is empty\n"); errs++; }
    if (!h.isa[0])      { printf("!! isa string is empty\n"); errs++; }

    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    fseek(f, (long)sizeof h, SEEK_SET);
    uint64_t payload = (uint64_t)fsz - sizeof h;
    printf("file size      : %ld  (payload %" PRIu64 " = %" PRIu64 " recs of %u B)\n",
           fsz, payload, h.rec_bytes ? payload / h.rec_bytes : 0, h.rec_bytes);
    if (h.rec_bytes == 0 || payload % h.rec_bytes) { printf("!! payload is not a whole number of records\n"); errs++; }
    if (h.rec_bytes) {
        uint64_t want = h.n_records * h.rec_bytes + h.shadow_bytes;
        printf("               = %" PRIu64 " correct-path + %" PRIu64 " shadow\n",
               h.n_records * h.rec_bytes, h.shadow_bytes);
        if (payload != want) {
            printf("!! payload %" PRIu64 " != n_records*rec_bytes + shadow_bytes = %" PRIu64 "\n",
                   payload, want); errs++;
        }
    }

    static const uint8_t want_lat[16] = {1,3,12,4,1,1,1,1,1,1,1,1,1,1,1,1};
    uint64_t cls[16] = {0};
    uint64_t n = 0, fe_sum = 0, mem_sum = 0, blkend = 0, badlat = 0, badmem = 0;
    uint64_t src1v = 0, src2v = 0, dstv = 0, maxreg = 0;
    uint64_t bit6_set = 0, n_call = 0, n_ret = 0, bad_ras = 0;
    uint64_t first_fe_delta = 0, first_mem_delta = 0;
    uint64_t fe_cursor = 0, mem_cursor = 0;          // running sum of deltas
    uint64_t fe_idx_max = 0, mem_idx_max = 0;        // max index actually used
    uint64_t memclass_recs = 0, mem_oob = 0, fe_oob = 0;
    bool     first_rec = true;
    uint64_t blk_run = 0, blk_hist[17] = {0};

    std::string buf; buf.resize((size_t)h.rec_bytes * 65536);
    uint64_t remaining = h.n_records;
    std::vector<std::pair<uint32_t,uint16_t>> shrefs;   // (shadow_off, shadow_len)
    uint64_t n_shadow_recs = 0;
    for (; remaining; ) {
        size_t want = (size_t)std::min<uint64_t>(remaining, buf.size() / h.rec_bytes);
        size_t got = fread(&buf[0], 1, want * h.rec_bytes, f);
        if (!got) break;
        size_t cnt = got / h.rec_bytes;
        remaining -= cnt;
        for (size_t i = 0; i < cnt; i++) {
            bstf_rec_t r; memset(&r, 0, sizeof r);
            memcpy(&r, &buf[i * h.rec_bytes], h.rec_bytes < sizeof r ? h.rec_bytes : sizeof r);
            cls[r.uop_class]++;
            // ---- CONTRACT G4 reader simulation ----
            if (first_rec) { first_fe_delta = r.fe_index_delta;
                             first_mem_delta = r.mem_index_delta; first_rec = false; }
            fe_cursor  += r.fe_index_delta;
            mem_cursor += r.mem_index_delta;
            {
                uint64_t fe_idx  = fe_cursor  - first_fe_delta;
                uint64_t mem_idx = mem_cursor - first_mem_delta;
                if (fe_idx > fe_idx_max) fe_idx_max = fe_idx;
                if (fe_idx >= h.n_fe_blocks) fe_oob++;
                bool memclass = (r.uop_class == UC_LOAD || r.uop_class == UC_STORE
                                 || r.uop_class == UC_AMO);
                if (memclass) {
                    memclass_recs++;
                    if (mem_idx > mem_idx_max) mem_idx_max = mem_idx;
                    if (mem_idx >= h.n_mem_access) mem_oob++;
                }
            }
            if (r.exec_lat != want_lat[r.uop_class]) badlat++;
            fe_sum  += r.fe_index_delta;
            mem_sum += r.mem_index_delta;
            if (r.mem_index_delta && r.uop_class != UC_LOAD && r.uop_class != UC_STORE
                && r.uop_class != UC_AMO) badmem++;
            if (r.is_block_end) blkend++;
            if (!!(r.flags & BF_BLK_END) != !!r.is_block_end) errs++;
            // A4: bit7 = valid, bit[5:0] = reg number, bit6 reserved (must be 0)
            if (r.src1 & 0x40) bit6_set++;
            if (r.src2 & 0x40) bit6_set++;
            if (r.dst  & 0x40) bit6_set++;
            if (r.src1 & 0x80) { src1v++; if ((r.src1 & 0x3f) > maxreg) maxreg = r.src1 & 0x3f; }
            if (r.src2 & 0x80) { src2v++; if ((r.src2 & 0x3f) > maxreg) maxreg = r.src2 & 0x3f; }
            if (r.dst  & 0x80) { dstv++;  if ((r.dst  & 0x3f) > maxreg) maxreg = r.dst  & 0x3f; }
            // A2: BF_CALL / BF_RET must only appear on control transfers
            if (r.flags & BF_CALL) { n_call++;
                if (r.uop_class != UC_JUMP && r.uop_class != UC_JALR) bad_ras++; }
            if (r.flags & BF_RET) { n_ret++;
                if (r.uop_class != UC_RET && r.uop_class != UC_JALR) bad_ras++; }
            blk_run++;
            if (r.is_block_end) { blk_hist[blk_run < 17 ? blk_run : 16]++; blk_run = 0; }
            if (r.shadow_off) shrefs.push_back(std::make_pair((uint32_t)r.shadow_off,
                                                              (uint16_t)r.shadow_len));
            n++;
        }
    }
    (void)n_shadow_recs;

    double N = (double)(n ? n : 1);
    printf("\n=== re-derived from records ===\n");
    printf("records read   : %" PRIu64 " %s\n", n, n == h.n_records ? "(matches header)" : "(MISMATCH)");
    if (n != h.n_records) errs++;
    printf("sum fe_delta   : %" PRIu64 " %s n_fe_blocks %" PRIu64 "\n",
           fe_sum, fe_sum == h.n_fe_blocks ? "==" : "!=", h.n_fe_blocks);
    if (fe_sum != h.n_fe_blocks) errs++;
    // ---- CONTRACT G4: idx(i) = sum_{j<=i} delta(j) - delta(0), 0-based ----
    printf("\n-- CONTRACT G4 overlay index check (same formula for .fe and .mem) --\n");
    uint64_t fe_expect  = h.n_fe_blocks  ? h.n_fe_blocks  - 1 : 0;
    uint64_t mem_expect = h.n_mem_access ? h.n_mem_access - 1 : 0;
    printf("  .fe  delta(0)=%" PRIu64 "  sum-delta(0)=%" PRIu64 " %s n_fe_blocks-1=%" PRIu64 "\n",
           first_fe_delta, fe_sum - first_fe_delta,
           (fe_sum - first_fe_delta) == fe_expect ? "==" : "!=", fe_expect);
    if ((fe_sum - first_fe_delta) != fe_expect) errs++;
    printf("  .mem delta(0)=%" PRIu64 "  sum-delta(0)=%" PRIu64 " %s n_mem_access-1=%" PRIu64 "\n",
           first_mem_delta, mem_sum - first_mem_delta,
           (mem_sum - first_mem_delta) == mem_expect ? "==" : "!=", mem_expect);
    if ((mem_sum - first_mem_delta) != mem_expect) errs++;
    printf("  highest .fe  index used: %" PRIu64 "  (must be %" PRIu64 ")   out-of-range: %" PRIu64 "\n",
           fe_idx_max, fe_expect, fe_oob);
    if (fe_idx_max != fe_expect || fe_oob) errs++;
    printf("  highest .mem index used: %" PRIu64 "  (must be %" PRIu64 ")   out-of-range: %" PRIu64 "\n",
           mem_idx_max, mem_expect, mem_oob);
    if (mem_idx_max != mem_expect || mem_oob) errs++;
    printf("  LOAD/STORE/AMO records : %" PRIu64 " %s n_mem_access %" PRIu64
           "   (consumers key off uop_class)\n",
           memclass_recs, memclass_recs == h.n_mem_access ? "==" : "!=", h.n_mem_access);
    if (memclass_recs != h.n_mem_access) errs++;
    printf("block ends     : %" PRIu64 " %s n_fe_blocks\n",
           blkend, blkend == h.n_fe_blocks ? "==" : "!=");
    if (blkend != h.n_fe_blocks) errs++;
    printf("avg block len  : %.3f\n", h.n_fe_blocks ? (double)n / (double)h.n_fe_blocks : 0.0);
    printf("exec_lat viol. : %" PRIu64 "\n", badlat);   if (badlat) errs++;
    printf("mem on non-mem : %" PRIu64 "\n", badmem);   if (badmem) errs++;
    printf("max reg number : %" PRIu64 " (bit[5:0], must be <= 63)\n", maxreg);
    if (maxreg > 63) errs++;
    printf("bit6 set (rsvd): %" PRIu64 " (must be 0)\n", bit6_set);  if (bit6_set) errs++;
    printf("BF_CALL / BF_RET: %" PRIu64 " / %" PRIu64 "   misplaced: %" PRIu64 "\n",
           n_call, n_ret, bad_ras);
    if (bad_ras) errs++;
    if (cls[UC_RET] && n_ret < cls[UC_RET]) { printf("!! UC_RET records without BF_RET\n"); errs++; }
    printf("src1/src2/dst valid: %" PRIu64 " / %" PRIu64 " / %" PRIu64 "\n", src1v, src2v, dstv);

    printf("\n-- class mix --\n");
    for (int i = 0; i < 16; i++)
        if (cls[i]) printf("  %-7s %12" PRIu64 "  %6.2f%%\n", kClassName[i], cls[i], 100.0*(double)cls[i]/N);
    uint64_t mem_cls = cls[UC_LOAD] + cls[UC_STORE] + cls[UC_AMO];
    uint64_t br_cls  = cls[UC_BRANCH] + cls[UC_JUMP] + cls[UC_JALR] + cls[UC_RET];
    printf("\nmemory ops     : %6.2f%%   (CoreMark reference 20-30%%)\n", 100.0*(double)mem_cls/N);
    printf("control ops    : %6.2f%%   (CoreMark reference 10-20%%)\n", 100.0*(double)br_cls/N);

    printf("\n-- block length histogram (from is_block_end) --\n");
    for (int i = 1; i < 17; i++)
        if (blk_hist[i]) printf("  %2d : %12" PRIu64 "  %6.2f%%\n", i, blk_hist[i],
                                100.0*(double)blk_hist[i]/(double)(h.n_fe_blocks?h.n_fe_blocks:1));

    // ---- companion text files ----
    auto count_lines = [](const char* p, uint64_t* total, uint64_t* ifetch) {
        FILE* g = fopen(p, "rb"); if (!g) { perror(p); return false; }
        static char b[1<<20]; uint64_t t = 0, fi = 0; size_t got;
        bool comment = false, at_bol = true;
        while ((got = fread(b, 1, sizeof b, g)) > 0)
            for (size_t i = 0; i < got; i++) {
                if (at_bol) { comment = (b[i] == '#'); at_bol = false; }
                if (b[i] == '\n') { if (!comment) t++; at_bol = true; }
            }
        fclose(g); *total = t; *ifetch = fi; return true;
    };
    // ------------------------------------------------------------------
    // wrong-path shadow region
    // ------------------------------------------------------------------
    printf("\n-- wrong-path shadow --\n");
    uint64_t corr_bytes = (uint64_t)sizeof(bstf_hdr_t) + h.n_records * h.rec_bytes;
    if (!h.shadow_bytes) {
        printf("  no shadow region (shadow_offset=%" PRIu64 " shadow_bytes=%" PRIu64 ")\n",
               h.shadow_offset, h.shadow_bytes);
        if (!shrefs.empty()) {
            printf("!! %zu records carry shadow_off but the header declares no shadow area\n",
                   shrefs.size()); errs++;
        }
    } else {
        printf("  shadow_offset          : %" PRIu64 " %s end of correct path (%" PRIu64 ")\n",
               h.shadow_offset, h.shadow_offset == corr_bytes ? "==" : "!=", corr_bytes);
        if (h.shadow_offset != corr_bytes) errs++;
        printf("  shadow_bytes           : %" PRIu64 "  (file has %" PRIu64 " after the records)\n",
               h.shadow_bytes, (uint64_t)fsz - corr_bytes);
        if ((uint64_t)fsz != corr_bytes + h.shadow_bytes) {
            printf("!! file size does not match header + records + shadow\n"); errs++;
        }
        if (h.shadow_bytes % h.rec_bytes) {
            printf("!! shadow area is not a whole number of records\n"); errs++;
        }
        uint64_t sh_recs = h.shadow_bytes / h.rec_bytes;
        printf("  shadow records         : %" PRIu64 "\n", sh_recs);
        printf("  records with a shadow  : %zu\n", shrefs.size());

        // the layout is fixed stride: block k starts at record k*stride
        uint64_t stride = shrefs.empty() ? 0 : sh_recs / shrefs.size();
        printf("  derived stride (K)     : %" PRIu64 "\n", stride);
        if (stride == 0 || sh_recs != stride * shrefs.size()) {
            printf("!! shadow area is not a fixed stride times the number of shadows\n"); errs++;
        } else {
            uint64_t bad_align = 0, bad_range = 0, bad_len = 0, bad_order = 0;
            for (size_t k = 0; k < shrefs.size(); k++) {
                uint64_t off = shrefs[k].first, len = shrefs[k].second;
                if (off < h.shadow_offset) { bad_range++; continue; }
                uint64_t ridx = (off - h.shadow_offset) / h.rec_bytes;
                if ((off - h.shadow_offset) % h.rec_bytes) bad_align++;
                if (ridx % stride) bad_align++;
                if (ridx / stride != k) bad_order++;          // CONTRACT G5
                if (off + len * h.rec_bytes > h.shadow_offset + h.shadow_bytes) bad_range++;
                if (len > stride) bad_len++;
            }
            printf("  shadow_off misaligned  : %" PRIu64 "\n", bad_align);   if (bad_align) errs++;
            printf("  shadow block out of file: %" PRIu64 "\n", bad_range);  if (bad_range) errs++;
            printf("  shadow_len > stride    : %" PRIu64 "\n", bad_len);     if (bad_len) errs++;
            printf("  G5 k mismatch          : %" PRIu64 "  (k must equal (shadow_off-shadow_offset)/rec_bytes/K)\n",
                   bad_order);                                               if (bad_order) errs++;
        }

        // walk the shadow records themselves, block by block, so padding is
        // checked *positionally* rather than guessed from its contents (a real
        // wrong-path c.nop looks exactly like a pad record).
        fseek(f, (long)h.shadow_offset, SEEK_SET);
        uint64_t scls[16] = {0}, spad_ok = 0, spad_bad = 0, sblkend = 0,
                 sbit6 = 0, sbadlat = 0, sreal = 0;
        uint64_t left = sh_recs, recidx = 0;
        std::vector<uint8_t> shmem(sh_recs, 0);   // bit0 = memory class, bit1 = store
        while (left) {
            size_t want2 = (size_t)std::min<uint64_t>(left, buf.size() / h.rec_bytes);
            size_t got = fread(&buf[0], 1, want2 * h.rec_bytes, f);
            if (!got) break;
            size_t cnt = got / h.rec_bytes; left -= cnt;
            for (size_t i = 0; i < cnt; i++, recidx++) {
                bstf_rec_t r; memset(&r, 0, sizeof r);
                memcpy(&r, &buf[i * h.rec_bytes], h.rec_bytes);
                uint64_t k = recidx / stride, j = recidx % stride;
                uint16_t len = (k < shrefs.size()) ? shrefs[k].second : 0;
                if (j >= len) {
                    bool canon = (r.uop_class == UC_NOP) && (r.exec_lat == 1)
                              && !r.src1 && !r.src2 && !r.dst && !r.flags
                              && !r.mem_index_delta && !r.fe_index_delta
                              && !r.is_block_end;
                    if (canon) spad_ok++; else spad_bad++;
                    continue;
                }
                sreal++;
                if (r.uop_class == UC_LOAD || r.uop_class == UC_STORE || r.uop_class == UC_AMO)
                    shmem[recidx] = (uint8_t)(1 | (r.mem_store ? 2 : 0));
                scls[r.uop_class]++;
                if (r.is_block_end) sblkend++;
                if ((r.src1 | r.src2 | r.dst) & 0x40) sbit6++;
                if (r.exec_lat != want_lat[r.uop_class]) sbadlat++;
                if (r.shadow_off || r.shadow_len) { printf("!! nested shadow\n"); errs++; }
            }
        }
        printf("  shadow records read    : %" PRIu64 " %s\n", sh_recs - left,
               left == 0 ? "(all)" : "(SHORT)");
        if (left) errs++;
        printf("  real shadow instructions: %" PRIu64 "\n", sreal);
        printf("  padding slots          : %" PRIu64 " canonical, %" PRIu64 " NOT canonical\n",
               spad_ok, spad_bad);
        if (spad_bad) errs++;
        printf("  shadow block ends      : %" PRIu64 "\n", sblkend);
        printf("  bit6 set / exec_lat viol.: %" PRIu64 " / %" PRIu64 "\n", sbit6, sbadlat);
        if (sbit6 || sbadlat) errs++;
        printf("  shadow class mix (real):");
        for (int i = 0; i < 16; i++) if (scls[i]) printf(" %s=%.1f%%", kClassName[i],
                100.0 * (double)scls[i] / (double)(sreal ? sreal : 1));
        printf("\n");
        // every shadow_len must be <= stride and the real instructions must be
        // the non-padding prefix of their block
        // ---- .mem.wp.txt must reference real memory-class shadow records ----
        if (memwp_path) {
            FILE* g2 = fopen(memwp_path, "rb");
            if (!g2) perror(memwp_path);
            else {
                static char bb[1 << 20];
                std::string ln2; ln2.reserve(128);
                uint64_t rows = 0, bad_idx = 0, bad_cls = 0, bad_st = 0, bad_ord = 0, ncol = 0;
                uint64_t prev_idx = 0; bool first2 = true;
                auto row = [&](const std::string& L) {
                    if (L.empty() || L[0] == '#') return;
                    // vaddr and pc are hex, the counters are decimal
                    static const int base[7] = {16, 10, 10, 10, 16, 10, 10};
                    uint64_t f2[7]; int nf = 0; const char* p3 = L.c_str();
                    while (*p3 && nf < 7) { while (*p3 == ' ') p3++; if (!*p3) break;
                        char* e3; f2[nf] = strtoull(p3, &e3, base[nf]);
                        if (e3 == p3) break; nf++; p3 = e3; }
                    if (nf != 7) { ncol++; return; }
                    rows++;
                    uint64_t sidx = f2[5];
                    if (sidx >= sh_recs) { bad_idx++; return; }
                    if (!(shmem[sidx] & 1)) bad_cls++;
                    else if (((shmem[sidx] >> 1) & 1) != (f2[2] & 1)) bad_st++;
                    if (!first2 && sidx < prev_idx) bad_ord++;
                    prev_idx = sidx; first2 = false;
                };
                size_t g;
                while ((g = fread(bb, 1, sizeof bb, g2)) > 0)
                    for (size_t i = 0; i < g; i++) {
                        if (bb[i] == '\n') { row(ln2); ln2.clear(); } else ln2.push_back(bb[i]);
                    }
                if (!ln2.empty()) row(ln2);
                fclose(g2);
                printf("  mem.wp.txt rows        : %" PRIu64 "\n", rows);
                printf("    malformed rows       : %" PRIu64 "\n", ncol);         if (ncol) errs++;
                printf("    shadow_rec_idx out of range : %" PRIu64 "\n", bad_idx); if (bad_idx) errs++;
                printf("    referenced record is not LOAD/STORE/AMO : %" PRIu64 "\n", bad_cls);
                if (bad_cls) errs++;
                printf("    is_store disagrees with the record : %" PRIu64 "\n", bad_st);
                if (bad_st) errs++;
                printf("    shadow_rec_idx not monotonic : %" PRIu64 "\n", bad_ord); if (bad_ord) errs++;
            }
        }

        uint64_t sumlen = 0; for (auto& p2 : shrefs) sumlen += p2.second;
        printf("  sum(shadow_len)        : %" PRIu64 " %s real records %" PRIu64 "\n",
               sumlen, sumlen == sreal ? "==" : "!=", sreal);
        if (sumlen != sreal) errs++;
        printf("  sum(shadow_len)+padding: %" PRIu64 " %s shadow records %" PRIu64 "\n",
               sumlen + spad_ok + spad_bad,
               (sumlen + spad_ok + spad_bad) == sh_recs ? "==" : "!=", sh_recs);
        if (sumlen + spad_ok + spad_bad != sh_recs) errs++;
    }

    if (fe_path) {
        // Full scan: count rows, check the column count, and verify that
        // column 7 (next_pc) of every row equals column 1 (pc) of the next
        // row -- the property Agent A's fetch redirect relies on.
        FILE* g = fopen(fe_path, "rb");
        if (!g) perror(fe_path);
        else {
            static char b[1 << 20];
            std::string line; line.reserve(128);
            uint64_t rows = 0, chain_err = 0, ncol_err = 0, last_nextpc = 0;
            uint64_t prev_next = 0; bool have_prev = false;
            uint64_t tail_zero = 1;
            size_t got;
            auto handle = [&](const std::string& ln) {
                if (ln.empty() || ln[0] == '#') return;
                uint64_t f[7]; int nf = 0;
                const char* p2 = ln.c_str();
                while (*p2 && nf < 7) {
                    while (*p2 == ' ') p2++;
                    if (!*p2) break;
                    char* e2;
                    f[nf++] = strtoull(p2, &e2, 16);
                    if (e2 == p2) break;
                    p2 = e2;
                }
                if (nf != 7) { ncol_err++; return; }
                rows++;
                if (have_prev && prev_next != f[0]) chain_err++;
                prev_next = f[6]; have_prev = true;
                last_nextpc = f[6];
            };
            while ((got = fread(b, 1, sizeof b, g)) > 0)
                for (size_t i = 0; i < got; i++) {
                    if (b[i] == '\n') { handle(line); line.clear(); }
                    else line.push_back(b[i]);
                }
            if (!line.empty()) handle(line);
            fclose(g);
            // the final row has no successor, so its next_pc is never compared
            tail_zero = have_prev && (last_nextpc == 0);
            printf("\nfrontend.txt rows  : %" PRIu64 " %s n_records\n",
                   rows, rows == h.n_records ? "==" : "!=");
            if (rows != h.n_records) errs++;
            printf("  rows without 7 columns          : %" PRIu64 "\n", ncol_err);
            if (ncol_err) errs++;
            printf("  next_pc != next row's pc        : %" PRIu64 "  (the final row is excluded)\n",
                   chain_err);
            if (chain_err) errs++;
            printf("  final row next_pc == 0          : %s\n", tail_zero ? "yes" : "NO");
            if (!tail_zero) errs++;
        }
    }
    if (mem_path) {
        uint64_t t = 0, x = 0;
        if (count_lines(mem_path, &t, &x)) {
            printf("mem.txt rows       : %" PRIu64 " %s n_mem_access %" PRIu64 "\n",
                   t, t == h.n_mem_access ? "==" : "!=", h.n_mem_access);
            if (t != h.n_mem_access) errs++;
        }
    }
    if (imem_path) {
        uint64_t t = 0, x = 0;
        if (count_lines(imem_path, &t, &x)) {
            printf("imem.txt rows      : %" PRIu64 "  (>= n_fe_blocks %" PRIu64 ", extras are line straddles)\n",
                   t, h.n_fe_blocks);
            if (t < h.n_fe_blocks) errs++;
        }
    }

    printf("\n==> %s (%d problem%s)\n", errs ? "FAIL" : "PASS", errs, errs == 1 ? "" : "s");
    return errs ? 1 : 0;
}
