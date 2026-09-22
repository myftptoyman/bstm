#define _GNU_SOURCE
#include "bstf_reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

static __thread char g_err[768];
const char *bstf_strerror(void) { return g_err; }

static const uint8_t *map_file(const char *p, size_t *len, int required)
{
    int fd = open(p, O_RDONLY);
    struct stat st;
    void *m;
    if (fd < 0) {
        if (required) snprintf(g_err, sizeof g_err, "open %s: %s", p, strerror(errno));
        *len = 0; return NULL;
    }
    if (fstat(fd, &st) < 0 || st.st_size == 0) {
        close(fd); *len = 0;
        if (required) snprintf(g_err, sizeof g_err, "stat %s: empty", p);
        return NULL;
    }
    m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);                       /* mapping 會保留 */
    if (m == MAP_FAILED) {
        snprintf(g_err, sizeof g_err, "mmap %s: %s", p, strerror(errno));
        *len = 0; return NULL;
    }
    /* trace 是循序大量讀取，read-mostly */
    madvise(m, (size_t)st.st_size, MADV_WILLNEED);
    *len = (size_t)st.st_size;
    return (const uint8_t *)m;
}

static char *read_text(const char *p, size_t *len)
{
    FILE *f = fopen(p, "rb");
    char *b; long n;
    *len = 0;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    b = malloc((size_t)n + 1);
    if (!b) { fclose(f); return NULL; }
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
    b[n] = 0; *len = (size_t)n; fclose(f);
    return b;
}

/* 極簡 flat-JSON 取值："key" : <value>  —— 只支援字串與數字。 */
int bstf_meta_get(const bstf_trace_t *t, const char *key, char *buf, size_t n)
{
    char pat[128];
    const char *p, *q;
    size_t i = 0;
    if (!t->meta) return -1;
    snprintf(pat, sizeof pat, "\"%s\"", key);
    p = strstr(t->meta, pat);
    if (!p) return -1;
    p = strchr(p + strlen(pat), ':');
    if (!p) return -1;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n') p++;
    if (*p == '"') {
        p++; q = strchr(p, '"');
        if (!q) return -1;
    } else {
        q = p;
        while (*q && *q != ',' && *q != '}' && *q != '\n' && *q != ' ') q++;
    }
    while (p < q && i + 1 < n) buf[i++] = *p++;
    buf[i] = 0;
    return 0;
}

static int check_size(const char *what, uint64_t got, uint64_t want, int lax)
{
    if (got == want) return 0;
    snprintf(g_err, sizeof g_err,
             "%s: size %llu != expected %llu (overlay 沒有 magic，只能靠大小檢查)",
             what, (unsigned long long)got, (unsigned long long)want);
    return lax ? 0 : -1;
}

int bstf_open(bstf_trace_t *t, const char *base)
{
    /* 整合期間方便用：BSTF_LAX=1 可以在 overlay 還沒產生時先跑 .bstf */
    const char *e = getenv("BSTF_LAX");
    return bstf_open_ex(t, base, (e && *e && *e != '0') ? BSTF_LAX : BSTF_STRICT);
}

int bstf_open_ex(bstf_trace_t *t, const char *base, int mode)
{
    char p[512];
    size_t l;
    int lax = (mode == BSTF_LAX);
    memset(t, 0, sizeof *t);
    snprintf(t->path, sizeof t->path, "%s", base);

    snprintf(p, sizeof p, "%s.bstf", base);
    t->map = map_file(p, &t->map_len, 1);
    if (!t->map) return -1;
    if (t->map_len < sizeof(bstf_hdr_t)) {
        snprintf(g_err, sizeof g_err, "%s: too small", p);
        bstf_close(t); return -1;
    }
    t->hdr = (const bstf_hdr_t *)t->map;
    if (t->hdr->magic != BSTF_MAGIC) {
        snprintf(g_err, sizeof g_err, "%s: bad magic %llx",
                 p, (unsigned long long)t->hdr->magic);
        bstf_close(t); return -1;
    }
    if (t->hdr->version != BSTF_VERSION) {
        snprintf(g_err, sizeof g_err, "%s: version %u != %u",
                 p, t->hdr->version, BSTF_VERSION);
        bstf_close(t); return -1;
    }
    /* bstf.h v2 註記：一律以 hdr.rec_bytes 為準，不要用 sizeof()。
     * 只要不小於我們認得的欄位長度就能讀（更大 = 未來加欄位，我們忽略尾巴）。*/
    t->rec_bytes = t->hdr->rec_bytes ? t->hdr->rec_bytes : (uint32_t)sizeof(bstf_rec_t);
    if (t->rec_bytes < (uint32_t)sizeof(bstf_rec_t)) {
        snprintf(g_err, sizeof g_err, "%s: rec_bytes %u < %u（欄位不足）",
                 p, t->rec_bytes, (uint32_t)sizeof(bstf_rec_t));
        bstf_close(t); return -1;
    }
    t->rec       = (const bstf_rec_t *)(t->map + sizeof(bstf_hdr_t));
    t->n_records = t->hdr->n_records;
    t->cp_end    = sizeof(bstf_hdr_t) + t->n_records * (uint64_t)t->rec_bytes;
    if (t->cp_end > t->map_len) t->cp_end = t->map_len;

    snprintf(p, sizeof p, "%s.fe", base);
    t->fe = map_file(p, &l, 0);  t->n_fe  = l;
    if (check_size(".fe", t->n_fe, t->hdr->n_fe_blocks, lax) < 0) { bstf_close(t); return -1; }

    snprintf(p, sizeof p, "%s.mem", base);
    t->mem = map_file(p, &l, 0); t->n_mem = l;
    if (check_size(".mem", t->n_mem, t->hdr->n_mem_access, lax) < 0) { bstf_close(t); return -1; }

    /* .fe.wp 是 **block** 粒度（CONTRACT §19），不是指令粒度。
     * 檔頭沒有 D，所以用方案 (a)：從 .fe 數 FE_REDIRECT 得到誤預測次數 N，
     * 然後要求 n_fe_wp 是 N 的整數倍，商就是 D。
     * 這比單純比大小強：它同時驗證了「長度是誤預測次數的整數倍」。 */
    t->shadow_recs = t->rec_bytes ? t->hdr->shadow_bytes / t->rec_bytes : 0;
    if (t->fe) {
        uint64_t i;
        for (i = 0; i < t->n_fe; i++) if (t->fe[i] & FE_REDIRECT) t->n_mispred++;
    }
    snprintf(p, sizeof p, "%s.fe.wp", base);
    t->fe_wp = map_file(p, &l, 0);  t->n_fe_wp  = l;
    if (t->fe_wp) {
        if (t->n_mispred == 0) {
            snprintf(g_err, sizeof g_err,
                     ".fe.wp 有 %llu byte，但 .fe 裡沒有任何 FE_REDIRECT",
                     (unsigned long long)t->n_fe_wp);
            if (!lax) { bstf_close(t); return -1; }
        } else if (t->n_fe_wp % t->n_mispred) {
            snprintf(g_err, sizeof g_err,
                     ".fe.wp 長度 %llu 不是誤預測次數 %llu 的整數倍"
                     "（.fe.wp 是 block 粒度，D = 長度/次數）",
                     (unsigned long long)t->n_fe_wp, (unsigned long long)t->n_mispred);
            if (!lax) { bstf_close(t); return -1; }
        } else {
            t->fe_wp_depth = t->n_fe_wp / t->n_mispred;
        }
    }
    /* shadow 區的 stride（記錄數）。shadow_len 只數「真指令」，後面是 UC_NOP
     * padding，所以 k = wp_idx / stride，不是 wp_idx / shadow_len。 */
    if (t->n_mispred && t->shadow_recs) {
        if (t->shadow_recs % t->n_mispred) {
            snprintf(g_err, sizeof g_err,
                     "shadow 記錄數 %llu 不是誤預測次數 %llu 的整數倍"
                     "（無法推導 stride）",
                     (unsigned long long)t->shadow_recs,
                     (unsigned long long)t->n_mispred);
            if (!lax) { bstf_close(t); return -1; }
        } else {
            t->shadow_stride = t->shadow_recs / t->n_mispred;
        }
    }

    snprintf(p, sizeof p, "%s.mem.wp", base);
    t->mem_wp = map_file(p, &l, 0); t->n_mem_wp = l;

    snprintf(p, sizeof p, "%s.imem", base);       /* v2 B2：保留，demo 不接線 */
    t->imem = map_file(p, &l, 0); t->n_imem = l;

    snprintf(p, sizeof p, "%s.meta.json", base);
    t->meta = read_text(p, &t->meta_len);
    return 0;
}

void bstf_close(bstf_trace_t *t)
{
    if (t->map) munmap((void *)t->map, t->map_len);
    if (t->fe)  munmap((void *)t->fe,  (size_t)t->n_fe);
    if (t->mem) munmap((void *)t->mem, (size_t)t->n_mem);
    if (t->fe_wp)  munmap((void *)t->fe_wp,  (size_t)t->n_fe_wp);
    if (t->mem_wp) munmap((void *)t->mem_wp, (size_t)t->n_mem_wp);
    if (t->imem)   munmap((void *)t->imem,   (size_t)t->n_imem);
    free(t->meta);
    memset(t, 0, sizeof *t);
}

/* ---------------- 游標 ---------------- */

void bstf_cur_init(const bstf_trace_t *t, bstf_cursor_t *c)
{
    memset(c, 0, sizeof *c);
    c->rec_byte = sizeof(bstf_hdr_t);
    (void)t;
}

void bstf_cur_seek_byte(bstf_cursor_t *c, uint64_t byte_off,
                        uint64_t fe_idx, uint64_t mem_idx)
{
    memset(c, 0, sizeof *c);
    c->rec_byte = byte_off;
    c->fe_idx   = fe_idx;
    c->mem_idx  = mem_idx;
}

uint64_t bstf_cur_index(const bstf_trace_t *t, const bstf_cursor_t *c)
{
    return (c->rec_byte - sizeof(bstf_hdr_t)) / t->rec_bytes;
}

int bstf_cur_seek_index(const bstf_trace_t *t, bstf_cursor_t *c, uint64_t idx)
{
    uint64_t i;
    if (idx > t->n_records) return -1;
    bstf_cur_init(t, c);
    for (i = 1; i <= idx; i++) {         /* rec[0] 的 delta 定義為 0 */
        c->fe_idx  += t->rec[i].fe_index_delta;
        c->mem_idx += t->rec[i].mem_index_delta;
    }
    c->rec_byte = sizeof(bstf_hdr_t) + idx * (uint64_t)t->rec_bytes;
    return 0;
}






/* ---------------- wrong-path ---------------- */

int bstf_enter_shadow(const bstf_trace_t *t, bstf_cursor_t *c)
{
    const bstf_rec_t *r = bstf_peek(t, c);
    bstf_cursor_t resume;
    if (!r || c->in_shadow) return 0;
    if (r->shadow_off == 0 || r->shadow_len == 0) return 0;
    if ((uint64_t)r->shadow_off + t->rec_bytes > t->map_len) return 0;

    /* 續行點 = 分支記錄的下一筆 */
    resume = *c;
    bstf_advance(t, &resume, 1);

    c->sv_rec_byte = resume.rec_byte;
    c->sv_fe_idx   = resume.fe_idx;
    c->sv_mem_idx  = resume.mem_idx;

    c->rec_byte    = r->shadow_off;
    c->in_shadow   = 1;
    c->shadow_left = r->shadow_len;
    /* shadow 區裡的記錄序號（.mem.wp 用，指令粒度） */
    c->wp_idx = (t->hdr->shadow_offset && r->shadow_off >= t->hdr->shadow_offset)
              ? (r->shadow_off - t->hdr->shadow_offset) / t->rec_bytes : 0;
    /* .fe.wp 用：k = 誤預測序號 = shadow 記錄序號 / stride，基底 = k*D。
     * 用 stride 而不是 shadow_len —— shadow_len 只數真指令，後面有 padding。 */
    c->wp_fe_blk  = 0;
    {
        uint64_t stride = t->shadow_stride ? t->shadow_stride : r->shadow_len;
        c->wp_fe_base = (stride && t->fe_wp_depth)
                      ? (c->wp_idx / stride) * t->fe_wp_depth : 0;
    }
    return 1;
}

void bstf_leave_shadow(const bstf_trace_t *t, bstf_cursor_t *c)
{
    (void)t;
    if (!c->in_shadow) return;
    c->rec_byte    = c->sv_rec_byte;
    c->fe_idx      = c->sv_fe_idx;
    c->mem_idx     = c->sv_mem_idx;
    c->in_shadow   = 0;
    c->shadow_left = 0;
    c->wp_idx      = 0;
    c->wp_fe_base  = 0;
    c->wp_fe_blk   = 0;
}

/* ---------------- 欄位打包 ---------------- */
/* 對齊 model/ooo/common/ifc.vh：
 *   [31] wrongpath [30] blkend [29] serialize [28] memstore
 *   [27:24] class  [23:20] lat [19] s1v [18:13] s1
 *   [12] s2v [11:6] s2  [5] dv [4:0] d
 * 注意 d 只有 5 bit（ifc.vh 的 demo 限制 x0..x31），src 卻有 6 bit。 */
uint32_t bstf_rec_to_duop(const bstf_rec_t *r, int wrongpath)
{
    uint32_t d = 0;
    d |= (uint32_t)(wrongpath & 1) << 31;
    d |= (uint32_t)((r->is_block_end || (r->flags & BF_BLK_END)) ? 1u : 0u) << 30;
    d |= (uint32_t)((r->flags & BF_SERIALIZE) ? 1u : 0u) << 29;
    d |= (uint32_t)(r->mem_store & 1u) << 28;
    d |= (uint32_t)(r->uop_class & 0xFu) << 24;
    d |= (uint32_t)(r->exec_lat  & 0xFu) << 20;
    d |= (uint32_t)((r->src1 >> 7) & 1u) << 19;
    d |= (uint32_t)(r->src1 & 0x3Fu) << 13;
    d |= (uint32_t)((r->src2 >> 7) & 1u) << 12;
    d |= (uint32_t)(r->src2 & 0x3Fu) << 6;
    d |= (uint32_t)((r->dst  >> 7) & 1u) << 5;
    d |= (uint32_t)(r->dst & 0x1Fu);
    return d;
}
