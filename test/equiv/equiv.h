/* ============================================================
 * equiv.h — Verilator <-> bit-sliced 逐 cycle 對拍的共用報告器
 * PLAN §9.2 第一層驗證。第一個不同就報 cycle 號 + 訊號名 + 兩邊的值。
 * ============================================================ */
#ifndef BSTM_EQUIV_H
#define BSTM_EQUIV_H
#include <cstdio>
#include <cstdint>
#include <cstring>

struct EquivCtx {
    long        cycle   = 0;
    long        checks  = 0;
    int         fails   = 0;
    int         max_report = 5;
    const char *phase   = "";

    void set_phase(const char *p) { phase = p; }

    bool cmp(const char *sig, uint64_t ref, uint64_t dut, int width = 64)
    {
        checks++;
        uint64_t m = (width >= 64) ? ~0ULL : ((1ULL << width) - 1);
        if ((ref & m) == (dut & m)) return true;
        if (fails < max_report) {
            std::fprintf(stderr,
                "MISMATCH  phase=%s  cycle=%ld  signal=%s\n"
                "          verilator = 0x%llx (%llu)\n"
                "          bit-sliced= 0x%llx (%llu)\n"
                "          xor       = 0x%llx\n",
                phase, cycle, sig,
                (unsigned long long)(ref & m), (unsigned long long)(ref & m),
                (unsigned long long)(dut & m), (unsigned long long)(dut & m),
                (unsigned long long)((ref ^ dut) & m));
        }
        fails++;
        return false;
    }

    int report(const char *name) const
    {
        std::printf("%-28s %8ld cycles, %9ld signal-compares, %d mismatch%s\n",
                    name, cycle, checks, fails, fails == 1 ? "" : "es");
        return fails ? 1 : 0;
    }
};

/* 從 bit-sliced word 陣列取出 lane l 的 n-bit 純量值 */
static inline uint64_t bs_get(const uint64_t *w, int n, int lane)
{
    uint64_t v = 0;
    for (int b = 0; b < n; b++) v |= ((w[b] >> lane) & 1ULL) << b;
    return v;
}
/* 把純量值放進 bit-sliced word 陣列的 lane l */
static inline void bs_put(uint64_t *w, int n, int lane, uint64_t v)
{
    for (int b = 0; b < n; b++) {
        uint64_t m = 1ULL << lane;
        w[b] = (w[b] & ~m) | (((v >> b) & 1ULL) << lane);
    }
}

/* 簡單 LFSR（x^64 + x^63 + x^61 + x^60 + 1） */
struct Lfsr {
    uint64_t s;
    explicit Lfsr(uint64_t seed = 0x123456789abcdefULL) : s(seed ? seed : 1) {}
    uint64_t next() { s = (s << 1) ^ (-(int64_t)(s >> 63) & 0x1BULL); return s; }
};
#endif
