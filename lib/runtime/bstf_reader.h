/* ============================================================
 * bstf_reader.h — .bstf / .fe / .mem 的 mmap 讀取器
 *
 * 三個檔案、三個獨立游標：
 *   <base>.bstf   bstf_hdr_t + correct-path 記錄 + shadow 區
 *   <base>.fe     1 byte / fetch block
 *   <base>.mem    1 byte / memory access
 *
 * 游標推進規則（CONTRACT §5 / 監督者 2026-09-21 裁決，.fe 與 .mem 同一條公式）：
 *   idx(i) = sum_{j<=i} delta(j) - delta(0)
 * 本檔的實作方式：cursor 從 0 起跳，只在「從第 i-1 筆走到第 i 筆」時加
 * delta(i)，也就是永遠不加 delta(0) —— 與上式完全等價，而且不需要先讀
 * rec[0]。因此 delta(0) 是 0（.mem 的實際情況）或 1（.fe 的實際情況）
 * 都會得到 idx(0) == 0。
 *
 * ★ 重要（2026-09-21 裁決的副作用）：
 *   第一筆有記憶體存取的指令，它的 mem_index_delta 是 0（因為它就是索引 0），
 *   所以 **delta != 0 不能拿來判斷「這筆記錄要消耗一個 .mem entry」**。
 *   要用 uop_class ∈ {UC_LOAD, UC_STORE, UC_AMO} 判斷 —— 見 bstf_rec_has_mem()。
 *
 * wrong-path：rec.shadow_off 是「相對於 .bstf 檔案起點的位元組偏移」，
 * shadow_len 是該 shadow 的記錄數。依 bstf.h v2 A1/B2，.fe/.mem 只含
 * correct-path，所以進 shadow 之後 fe/mem 游標**凍結**，事件改從
 * .fe.wp / .mem.wp 取。離開 shadow 時整組還原。
 * （.fe.wp 不存在時退回沿用 correct-path 的 fe 事件，好讓舊 trace 還能跑。）
 *
 * ★ overlay 的粒度各不相同（CONTRACT §19，監督者 2026-09-22 裁決）：
 *     base .bstf  每「指令」一筆 16 byte
 *     .fe         每「fetch block」一 byte
 *     .fe.wp      每「wrong-path fetch block」一 byte   <- 不是每指令！
 *     .mem        每「data 存取」一 byte
 *   所以 .fe.wp 的索引是 k*D + (在這個 shadow 裡走過幾個 block)，
 *   k = 誤預測序號、D = wrongpath 深度（單位：block）。
 * ============================================================ */
#ifndef BSTM_BSTF_READER_H
#define BSTM_BSTF_READER_H

#include "bstm_types.h"
#include "bstf.h"

typedef struct {
    const uint8_t    *map;        /* .bstf 整個 mapping                  */
    size_t            map_len;
    const bstf_hdr_t *hdr;
    const bstf_rec_t *rec;        /* correct-path 記錄陣列（= map+hdr）   */
    uint64_t          n_records;
    uint32_t          rec_bytes;
    uint64_t          cp_end;     /* correct-path 區的結束位元組（避免熱路徑做除法） */

    const uint8_t    *fe;         /* .fe mapping（可為 NULL）            */
    uint64_t          n_fe;
    const uint8_t    *mem;        /* .mem mapping（可為 NULL）           */
    uint64_t          n_mem;
    /* bstf.h v2 A1/B2：wrong-path 的事件另外放 .fe.wp / .mem.wp，
     * 索引 = 該筆 shadow 記錄在 shadow 區裡的序號。 */
    const uint8_t    *fe_wp;   uint64_t n_fe_wp;
    const uint8_t    *mem_wp;  uint64_t n_mem_wp;
    /* .fe 裡有 FE_REDIRECT 的 block 數 = 誤預測次數 N。
     * .fe.wp 的長度必須是 N 的整數倍，商就是 D（單位：block）。 */
    uint64_t          n_mispred;     /* N */
    uint64_t          fe_wp_depth;   /* D（單位 block），0 = 沒有 .fe.wp */
    uint64_t          shadow_recs;   /* hdr.shadow_bytes / rec_bytes */
    /* shadow 區的排版：第 k 次誤預測佔記錄 [k*stride, (k+1)*stride)。
     * stride 通常 > shadow_len，多出來的是 UC_NOP padding，所以
     * **k 不能用 shadow_len 去除**，要用 stride。
     * stride = shadow_recs / n_mispred（可推導，不必解析 meta）。 */
    uint64_t          shadow_stride;
    /* v2 B2：instruction fetch overlay，demo 不接線，先保留 */
    const uint8_t    *imem;    uint64_t n_imem;
    /* v2 裁決 5：<base>.meta.json sidecar（配置指紋），可為 NULL */
    char             *meta;    size_t   meta_len;

    char              path[512];
} bstf_trace_t;

/* 同時支援「記錄索引」與「位元組游標」兩種模式：
 *   rec_byte 永遠是權威值（未來換變長格式時只有它有意義）
 *   rec_index 只在定長且位於 correct-path 區時有效
 */
typedef struct {
    uint64_t rec_byte;        /* 相對 .bstf 起點的位元組偏移 */
    uint64_t fe_idx;
    uint64_t mem_idx;

    uint8_t  in_shadow;
    uint32_t shadow_left;     /* 還可以讀幾筆 shadow        */
    uint64_t wp_idx;          /* .mem.wp 的索引（每 shadow 指令一 byte） */
    uint64_t wp_fe_base;      /* = k * fe_wp_depth          */
    uint32_t wp_fe_blk;       /* 在這個 shadow 裡走過幾個 block（0..D-1） */

    /* 進 shadow 前的續行點（= 分支記錄的下一筆） */
    uint64_t sv_rec_byte, sv_fe_idx, sv_mem_idx;
} bstf_cursor_t;

/* ---- 開關檔 ---- */
/* overlay 檔（.fe/.mem/.imem/.fe.wp）都沒有 magic/version，只能靠大小檢查：
 *   .fe    大小必須 == hdr.n_fe_blocks
 *   .mem   大小必須 == hdr.n_mem_access
 *   .fe.wp 大小必須 == hdr.shadow_bytes / rec_bytes（每筆 shadow 記錄一 byte）
 * 不符一律報錯退出，不做靜默錯位。測試可用 BSTF_LAX 放寬。 */
#define BSTF_STRICT 0
#define BSTF_LAX    1
int  bstf_open (bstf_trace_t *t, const char *base_path);  /* base 不含副檔名 */
int  bstf_open_ex(bstf_trace_t *t, const char *base_path, int mode);
/* 從 .meta.json 取一個 flat key 的值（字串或數字）。找不到回 -1。 */
int  bstf_meta_get(const bstf_trace_t *t, const char *key, char *buf, size_t n);
void bstf_close(bstf_trace_t *t);
const char *bstf_strerror(void);

/* ---- 游標 ---- */
void     bstf_cur_init(const bstf_trace_t *t, bstf_cursor_t *c);
/* O(n)：從頭累加 delta。只在 setup 用，主迴圈不要呼叫。 */
int      bstf_cur_seek_index(const bstf_trace_t *t, bstf_cursor_t *c, uint64_t idx);
/* 直接設位元組游標（變長格式模式）。fe/mem 由呼叫者負責。 */
void     bstf_cur_seek_byte(bstf_cursor_t *c, uint64_t byte_off,
                            uint64_t fe_idx, uint64_t mem_idx);
uint64_t bstf_cur_index(const bstf_trace_t *t, const bstf_cursor_t *c);

/* 以下四個在 refill 的熱路徑上（每筆記錄都會呼叫），所以放 header inline。
 * 特別注意不要用 bstf_cur_index()：它有一個整數除法。 */

/* 目前游標處是否還有記錄可讀 */
BSTM_INLINE int bstf_avail(const bstf_trace_t *t, const bstf_cursor_t *c)
{
    return c->in_shadow ? (c->shadow_left > 0) : (c->rec_byte < t->cp_end);
}

/* 讀取（不推進）。越界回 NULL。 */
BSTM_INLINE const bstf_rec_t *bstf_peek(const bstf_trace_t *t, const bstf_cursor_t *c)
{
    if (!bstf_avail(t, c)) return NULL;
    if (c->rec_byte + t->rec_bytes > t->map_len) return NULL;
    return (const bstf_rec_t *)(t->map + c->rec_byte);
}

/* 目前記錄對應的 fe / mem 事件位元組（越界回 0）。 */
BSTM_INLINE uint8_t bstf_fe_event(const bstf_trace_t *t, const bstf_cursor_t *c)
{
    if (c->in_shadow && t->fe_wp) {
        uint64_t i = c->wp_fe_base + c->wp_fe_blk;    /* block 粒度 */
        return (i < t->n_fe_wp) ? t->fe_wp[i] : 0;
    }
    return (t->fe && c->fe_idx < t->n_fe) ? t->fe[c->fe_idx] : 0;
}
/* 原始取值：不管這筆記錄有沒有記憶體存取，直接讀 mem_idx 指到的那個 byte。
 * 非記憶體指令的 mem_idx 指向「上一筆存取」，值是沒有意義的 ——
 * 一般請用 bstf_mem_event_of()。 */
BSTM_INLINE uint8_t bstf_mem_event(const bstf_trace_t *t, const bstf_cursor_t *c)
{
    if (c->in_shadow)
        return (t->mem_wp && c->wp_idx < t->n_mem_wp) ? t->mem_wp[c->wp_idx] : 0;
    return (t->mem && c->mem_idx < t->n_mem) ? t->mem[c->mem_idx] : 0;
}

/* 這筆記錄是不是真的會消耗一個 .mem entry。
 * 監督者裁決：這個判斷只能看 uop_class，不能看 mem_index_delta。
 * Agent H 的 bstf_check 斷言「這類記錄的數量 == hdr.n_mem_access」。 */
BSTM_INLINE int bstf_rec_has_mem(const bstf_rec_t *r)
{
    return r->uop_class == UC_LOAD || r->uop_class == UC_STORE ||
           r->uop_class == UC_AMO;
}

/* 帶記錄的版本：非記憶體指令一律回 0（MEM_LEVEL=L1 / LAT_CLASS=0 的安全值），
 * 不會把上一筆存取的事件洩漏給模型。 */
BSTM_INLINE uint8_t bstf_mem_event_of(const bstf_trace_t *t, const bstf_cursor_t *c,
                                      const bstf_rec_t *r)
{
    return bstf_rec_has_mem(r) ? bstf_mem_event(t, c) : 0;
}

/* 推進 n 筆。回傳實際推進的筆數（可能因為 EOF / shadow 用完而較少）。 */
BSTM_INLINE uint32_t bstf_advance(const bstf_trace_t *t, bstf_cursor_t *c, uint32_t n)
{
    uint32_t done = 0;
    while (done < n) {
        if (!bstf_avail(t, c)) break;
        if (c->in_shadow) {
            /* .fe.wp 是 block 粒度：只有跨過 block 邊界才前進，且夾在 D-1 */
            const bstf_rec_t *cr = (const bstf_rec_t *)(t->map + c->rec_byte);
            int be = cr->is_block_end || (cr->flags & BF_BLK_END);
            c->rec_byte += t->rec_bytes;
            c->shadow_left--;
            c->wp_idx++;                              /* .mem.wp 是指令粒度 */
            if (be && t->fe_wp_depth && c->wp_fe_blk + 1 < t->fe_wp_depth)
                c->wp_fe_blk++;
            done++;
            continue;
        }
        c->rec_byte += t->rec_bytes;
        /* 新位置的記錄帶著「相對上一筆」的 delta（不加 delta(0)） */
        if (c->rec_byte < t->cp_end && c->rec_byte + t->rec_bytes <= t->map_len) {
            const bstf_rec_t *nx = (const bstf_rec_t *)(t->map + c->rec_byte);
            c->fe_idx  += nx->fe_index_delta;
            c->mem_idx += nx->mem_index_delta;
        }
        done++;
    }
    return done;
}

/* ---- wrong-path ---- */
/* 游標必須停在分支記錄上。成功回 1（有 shadow 才會成功）。 */
int  bstf_enter_shadow(const bstf_trace_t *t, bstf_cursor_t *c);
/* 回到正確路徑（分支記錄的下一筆）。 */
void bstf_leave_shadow(const bstf_trace_t *t, bstf_cursor_t *c);

/* ---- 記錄 -> 模型的 DUOP 欄位（對齊 model/ooo/common/ifc.vh） ---- */
uint32_t bstf_rec_to_duop(const bstf_rec_t *r, int wrongpath);

#endif /* BSTM_BSTF_READER_H */
