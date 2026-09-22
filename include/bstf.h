/* ============================================================
 * BSTF — Bit-Sliced Timing Format          契約檔，不可擅改
 * 三種粒度，各自一個游標：
 *   base.bstf  指令流      16 byte / 指令
 *   .fe        fetch block  1 byte / block
 *   .mem       記憶體存取   1 byte / access
 * ============================================================ */
#ifndef BSTF_H
#define BSTF_H
#include <stdint.h>

#define BSTF_MAGIC   0x3146545342ULL   /* "BSTF1" */
#define BSTF_VERSION 2   /* v2: .fe 線性/separate wrong-path、BF_CALL/BF_RET、.mem 僅 data */

/* ---- uop class ---- */
enum {
    UC_ALU=0, UC_MUL=1, UC_DIV=2, UC_FPU=3, UC_LOAD=4, UC_STORE=5,
    UC_BRANCH=6, UC_JUMP=7, UC_JALR=8, UC_RET=9, UC_CSR=10,
    UC_FENCE=11, UC_AMO=12, UC_NOP=13, UC_SYS=14, UC_VEC=15
};
/* ---- flags ---- */
#define BF_SERIALIZE   0x01   /* 必須單獨執行 */
#define BF_FENCE       0x02
#define BF_BLK_END     0x04   /* 此指令是 fetch block 的最後一條 */
#define BF_INTERVAL_END 0x08  /* SimPoint 區間結束 */
#define BF_CALL        0x10   /* v2: 此 jump/jalr 是 call（推 RAS）。A2 裁決 */
#define BF_RET         0x20   /* v2: 此 jalr 是 return（彈 RAS）*/

/* 16 byte 定長指令記錄。欄位刻意壓窄以符合建模規則 1。 */
typedef struct __attribute__((packed)) {
    uint8_t  uop_class : 4;
    uint8_t  exec_lat  : 4;   /* 執行單元佔用拍數 0..15 */
    uint8_t  src1;            /* v2 權威定義：bit[5:0]=暫存器編號, bit7=valid, bit6 保留。
                                 A4 裁決：以本檔為準，PLAN §7.2 的 5-bit 說法作廢 */
    uint8_t  src2;
    uint8_t  dst;
    uint8_t  flags;
    uint8_t  mem_size  : 3;   /* log2 bytes */
    uint8_t  mem_store : 1;
    uint8_t  is_block_end : 1;
    uint8_t  rsvd0     : 3;
    uint16_t fe_index_delta;  /* 相對於上一筆的 fetch-block 序號增量 (0 或 1) */
    uint16_t mem_index_delta; /* 相對於上一筆的 mem-access 序號增量 (0 或 1) */
    uint32_t shadow_off;      /* wrong-path shadow 的「絕對檔案位元組偏移」；0 = 無。
                                 【已知上限】uint32 在 50M trace 時，correct-path 區就佔
                                 800 MB，只剩 K≈349 的餘裕；200M 指令的 trace 在任何 K 下
                                 都會溢位。產生端（bstf_gen）已加硬性失敗而非靜默 wrap。
                                 修法：改成相對於 hdr.shadow_offset（餘裕 ×5）。
                                 【尚未實作】—— 資料與 reader 目前都是絕對偏移。*/
    uint16_t shadow_len;      /* shadow 指令數 */
} bstf_rec_t;                 /* v2 修正：移除 rsvd1，現在真的是 16 bytes（H 發現原本是 18）。
                                 讀取仍應以 hdr.rec_bytes 為準，不要用 sizeof()。 */

/* fetch-block 事件（離線前端模擬器產生），1 byte */
#define FE_BUBBLES(b)    ((b) & 0x3)      /* 0..3 拍前端泡泡 */
#define FE_UBTB_HIT      0x04
#define FE_BTB_OVERRIDE  0x08
#define FE_DIR_OK        0x10             /* 方向預測正確 */
#define FE_TGT_OK        0x20             /* 目標預測正確 */
#define FE_WRONGPATH     0x40
#define FE_REDIRECT      0x80             /* 此 block 會觸發 redirect */
/* v2 A3 裁決：FE_BUBBLES==3 保留給「≥3 拍」或 I-cache miss 造成的 fetch bubble，
   目前離線前端模擬器不會產生，timing model 應視為 3 拍以上（飽和）。      */
/* v2 A1 裁決：.fe 只含 correct-path，線性、每 block 一 byte、游標可直接累加。
   wrong-path 另寫 <out>.fe.wp：第 k 次誤預測對應 byte 區間 [k*D, (k+1)*D)。
   v9 §19：D 的單位是 **fetch block** 不是指令（FE_* 是 block 層級屬性）。
   四種 overlay 三種粒度，不可互推：
     .fe / .fe.wp  每個 fetch block 一 byte
     .mem          每個 data 存取一 byte
     base .bstf    每條指令 16 byte                                   */
/* v2 B2 裁決：.mem 只含 data 存取（load/store/AMO），對應 mem_index_delta。
   instruction fetch 另寫 .imem（每 fetch block 一 byte）。demo 不接線。   */

/* 記憶體存取事件（離線 cache 模擬器產生），1 byte */
#define MEM_LEVEL(b)     ((b) & 0x3)      /* 0=L1 1=L2 2=L3 3=DRAM */
#define MEM_PREFETCH_HIT 0x04
#define MEM_TLB_MISS     0x08
#define MEM_LAT_CLASS(b) (((b)>>4) & 0xF) /* 無負載延遲 class，查表 */

typedef struct __attribute__((packed)) {
    uint64_t magic;
    uint32_t version;
    uint32_t rec_bytes;        /* 16 */
    uint64_t n_records;        /* correct-path 指令數 */
    uint64_t shadow_bytes;     /* shadow 區大小 */
    uint64_t shadow_offset;    /* shadow 區起始位元組 */
    uint64_t n_fe_blocks;
    uint64_t n_mem_access;
    uint32_t simpoint_id;
    uint32_t simpoint_weight;  /* 定點 Q16 */
    uint64_t icount_start;
    char     workload[64];
    char     isa[32];
    uint8_t  pad[64];
} bstf_hdr_t;

/* 無負載延遲查表（cycles），MEM_LAT_CLASS 的索引 */
static const uint8_t bstf_lat_table[16] = {
    /*0*/  3,   /* L1 hit            */
    /*1*/  4,
    /*2*/ 12,   /* L2 hit            */
    /*3*/ 16,
    /*4*/ 30,   /* L3 hit            */
    /*5*/ 40,
    /*6*/ 90,   /* DRAM              */
    /*7*/120,
    /*8*/160,
    /*9*/200,
    /*10*/2,    /* store buffer hit  */
    /*11*/8,    /* prefetch hit      */
    /*12*/50, /*13*/70, /*14*/140, /*15*/255
};
#endif
