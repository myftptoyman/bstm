#include "transpose.h"
#include <string.h>

/* ------------------------------------------------------------------
 * Hacker's Delight 遞迴交換法（§7-3 transpose32），推廣到 64x64。
 * 注意：HD 原版用 MSB-first 的 column 編號，直接照抄會得到「反對角轉置」
 *       （row i bit j -> row 63-j bit 63-i）。這裡把 shift 換到另一個
 *       運算元上，得到 LSB-first 的標準轉置：B[i] bit j == A[j] bit i。
 *   j = 32,16,8,4,2,1；每輪交換相距 j 的兩個 row 的對角子區塊。
 * 總共 6 輪 x 32 對 = 192 次 (xor,shift,and,xor,shift,xor) ~ 1150 ops。
 * ------------------------------------------------------------------ */
#if defined(__AVX2__)
#  include <immintrin.h>
#endif

/* 一輪 block swap：對所有 (k, k+j) 配對做
 *   t = ((A[k] >> j) ^ A[k+j]) & m;  A[k+j] ^= t;  A[k] ^= t << j;
 * 內層 i 在 [k, k+j) 是連續位址，所以 j >= 4 時可以用 AVX2 一次做 4 個。 */
#define ROUND_SCALAR(j, m)                                        \
    do {                                                          \
        int kk, ii;                                               \
        for (kk = 0; kk < 64; kk += 2 * (j))                      \
            for (ii = kk; ii < kk + (j); ii++) {                  \
                uint64_t t = ((A[ii] >> (j)) ^ A[ii + (j)]) & (m);\
                A[ii + (j)] ^= t;                                 \
                A[ii]       ^= t << (j);                          \
            }                                                     \
    } while (0)

#if defined(__AVX2__)
#define ROUND_AVX2(j, m)                                                       \
    do {                                                                       \
        const __m256i vm = _mm256_set1_epi64x((long long)(m));                 \
        int kk, ii;                                                            \
        for (kk = 0; kk < 64; kk += 2 * (j))                                   \
            for (ii = kk; ii < kk + (j); ii += 4) {                            \
                __m256i a = _mm256_loadu_si256((const __m256i *)&A[ii]);       \
                __m256i b = _mm256_loadu_si256((const __m256i *)&A[ii + (j)]); \
                __m256i t = _mm256_and_si256(                                  \
                    _mm256_xor_si256(_mm256_srli_epi64(a, (j)), b), vm);        \
                _mm256_storeu_si256((__m256i *)&A[ii + (j)],                   \
                                    _mm256_xor_si256(b, t));                   \
                _mm256_storeu_si256((__m256i *)&A[ii],                         \
                    _mm256_xor_si256(a, _mm256_slli_epi64(t, (j))));            \
            }                                                                  \
    } while (0)
#else
#define ROUND_AVX2(j, m) ROUND_SCALAR(j, m)
#endif

/* ------------------------------------------------------------------
 * Hacker's Delight 遞迴交換法（§7-3 transpose32），推廣到 64x64。
 *
 * 兩個相對原版的改動：
 *  1. HD 原版用 MSB-first 的 column 編號，直接照抄會得到「反對角轉置」
 *     （row i bit j -> row 63-j bit 63-i）。這裡把 shift 換到另一個
 *     運算元上，得到 LSB-first 的標準轉置：B[i] bit j == A[j] bit i。
 *  2. 原版的 k 迴圈用 k = (k|j)+1 & ~j 跳躍，相依鏈長、編譯器沒辦法
 *     展開。改成兩層迴圈之後內層位址連續，j >= 4 的四輪可以直接用
 *     AVX2 一次做 4 個 row（實測快 ~5 倍，見 test/bench）。
 * ------------------------------------------------------------------ */
void bstm_transpose64(uint64_t A[64])
{
    ROUND_AVX2(32, 0x00000000FFFFFFFFULL);
    ROUND_AVX2(16, 0x0000FFFF0000FFFFULL);
    ROUND_AVX2( 8, 0x00FF00FF00FF00FFULL);
    ROUND_AVX2( 4, 0x0F0F0F0F0F0F0F0FULL);
    ROUND_SCALAR(2, 0x3333333333333333ULL);
    ROUND_SCALAR(1, 0x5555555555555555ULL);
}

void bstm_transpose64_to(uint64_t dst[64], const uint64_t src[64])
{
    memcpy(dst, src, 64 * sizeof(uint64_t));
    bstm_transpose64(dst);
}

void bstm_pack_lanes(const uint64_t vals[64], vec_t *out, int nbits)
{
    uint64_t A[64];
    int b;
    memcpy(A, vals, sizeof A);
    bstm_transpose64(A);
    for (b = 0; b < nbits; b++) out[b] = (vec_t)A[b];
}

void bstm_unpack_lanes(const vec_t *in, uint64_t vals[64], int nbits)
{
    uint64_t A[64];
    int b;
    for (b = 0; b < nbits; b++) A[b] = (uint64_t)in[b];
    for (; b < 64; b++)         A[b] = 0;
    bstm_transpose64(A);
    memcpy(vals, A, sizeof A);
}

/* n <= 8 的特化。
 * 天真寫法是 64*n 次 shift/or，實測比完整 64x64 轉置還慢（分支/相依鏈長）。
 * 有 BMI2 就用 pdep/pext：一次處理 8 個 lane，
 *   pdep(bits, 0x0101010101010101) 把 8 個 bit 攤成 8 個 byte。
 * 每次呼叫只要 nbits*8 個 pdep。 */
#if defined(__BMI2__)
#  include <immintrin.h>
#  define BYTE_SPREAD 0x0101010101010101ULL
#endif

void bstm_unpack_lanes_small(const vec_t *in, uint8_t vals[64], int nbits)
{
#if defined(__BMI2__)
    uint64_t acc[8] = {0,0,0,0,0,0,0,0};
    int b, g;
    for (b = 0; b < nbits; b++) {
        uint64_t w = (uint64_t)in[b];
        for (g = 0; g < 8; g++)
            acc[g] |= _pdep_u64(w >> (8 * g), BYTE_SPREAD) << b;
    }
    memcpy(vals, acc, 64);
#else
    int b, l;
    for (l = 0; l < 64; l++) vals[l] = 0;
    for (b = 0; b < nbits; b++) {
        vec_t w = in[b];
        for (l = 0; l < 64; l++)
            vals[l] |= (uint8_t)(((w >> l) & 1u) << b);
    }
#endif
}

void bstm_pack_lanes_small(const uint8_t vals[64], vec_t *out, int nbits)
{
#if defined(__BMI2__)
    uint64_t src[8];
    int b, g;
    memcpy(src, vals, 64);
    for (b = 0; b < nbits; b++) {
        uint64_t w = 0;
        for (g = 0; g < 8; g++)
            w |= _pext_u64(src[g] >> b, BYTE_SPREAD) << (8 * g);
        out[b] = (vec_t)w;
    }
#else
    int b, l;
    for (b = 0; b < nbits; b++) {
        vec_t w = 0;
        for (l = 0; l < 64; l++)
            w |= (vec_t)((vals[l] >> b) & 1u) << l;
        out[b] = w;
    }
#endif
}

/* vals 是 [64][m]；對每個 m 做一次 64x64 轉置。 */
void bstm_pack_lanes_multi(const uint64_t *vals, int m, vec_t *out, int nbits)
{
    uint64_t A[64];
    int e, l, b;
    for (e = 0; e < m; e++) {
        for (l = 0; l < 64; l++) A[l] = vals[(size_t)l * m + e];
        bstm_transpose64(A);
        for (b = 0; b < nbits; b++) out[(size_t)e * nbits + b] = (vec_t)A[b];
    }
}
