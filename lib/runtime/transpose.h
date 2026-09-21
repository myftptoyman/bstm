/* ============================================================
 * transpose.h — 64x64 bit transpose (scalar/vector 邊界)
 *
 * 約定：
 *   "lane-major"  vals[l]      = lane l 的一個 <=64 bit 純量
 *   "bit-sliced"  words[b]     bit l = lane l 的第 b 個 bit
 *
 * pack   : lane-major -> bit-sliced
 * unpack : bit-sliced -> lane-major
 * 兩者都是同一個 64x64 轉置（轉置是自反的）。
 * ============================================================ */
#ifndef BSTM_TRANSPOSE_H
#define BSTM_TRANSPOSE_H

#include "bstm_types.h"

/* 原地 64x64 轉置。A[i] 的 bit j <-> A[j] 的 bit i。
 * Hacker's Delight §7-3 的遞迴交換法，6 輪。 */
void bstm_transpose64(uint64_t A[64]);

/* 非原地版本（dst 與 src 不可重疊）。 */
void bstm_transpose64_to(uint64_t dst[64], const uint64_t src[64]);

/* N x 64：只有前 n (<=64) 個 row 有效，其餘補零。
 *   pack   : vals[64] (每個取低 n bit) -> out[n]
 *   unpack : in[n] -> vals[64]
 * n < 64 時 out/in 只需要 n 個元素。 */
void bstm_pack_lanes  (const uint64_t vals[64], vec_t *out, int nbits);
void bstm_unpack_lanes (const vec_t *in, uint64_t vals[64], int nbits);

/* 小 n 特化（n <= 8）：直接 bit gather，比整個 64x64 轉置便宜。
 * refill 每 cycle 要取 fb_take(3 bit) 就用這個。 */
void bstm_unpack_lanes_small(const vec_t *in, uint8_t vals[64], int nbits);
void bstm_pack_lanes_small  (const uint8_t vals[64], vec_t *out, int nbits);

/* 一次轉置 m 組 <=64 bit 的值（例如 m 筆記錄 x 48 bit）。
 * vals 是 [64][m] 的 lane-major 陣列；out 是 [m][nbits] 的 bit-sliced。 */
void bstm_pack_lanes_multi(const uint64_t *vals, int m, vec_t *out, int nbits);

#endif /* BSTM_TRANSPOSE_H */
