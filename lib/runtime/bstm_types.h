/* ============================================================
 * BSTM runtime — 共用型別                         Agent G
 * ============================================================ */
#ifndef BSTM_TYPES_H
#define BSTM_TYPES_H

#include <stdint.h>
#include <stddef.h>

/* 一個 vec_t 的 bit l = lane l。demo backend 固定 64 lanes。 */
typedef uint64_t vec_t;

#define BSTM_LANES   64
#define VZERO        ((vec_t)0)
#define VONES        (~(vec_t)0)

/* 模型 fetch 介面寬度，必須與 model/ooo/common/ifc.vh 的 `W 一致 */
#ifndef BSTM_W
#define BSTM_W       4
#endif
/* 解碼後 uop 寬度，必須與 ifc.vh 的 `DUOP_W 一致 */
#ifndef BSTM_DUOP_W
#define BSTM_DUOP_W  32
#endif

/* 每筆 trace 記錄展開成 fetch buffer 的 bit 數：
 *   duop(32) + fe_event(8) + mem_event(8) = 48                */
#define BSTM_FE_W     8
#define BSTM_MEM_W    8
#define BSTM_ENTRY_W  (BSTM_DUOP_W + BSTM_FE_W + BSTM_MEM_W)   /* 48 */

/* counter 寬度（top.v 的 cnt_* 都是 48 bit） */
#define BSTM_CNT_W   48

#if defined(__GNUC__)
#  define BSTM_INLINE static inline __attribute__((always_inline))
#  define BSTM_LIKELY(x)   __builtin_expect(!!(x),1)
#  define BSTM_UNLIKELY(x) __builtin_expect(!!(x),0)
#else
#  define BSTM_INLINE static inline
#  define BSTM_LIKELY(x)   (x)
#  define BSTM_UNLIKELY(x) (x)
#endif

#endif /* BSTM_TYPES_H */
