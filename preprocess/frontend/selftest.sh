#!/bin/sh
# ============================================================
# selftest.sh — BSTM 離線前端模擬器自測（Agent A）
#   1. 用合成的巢狀迴圈 + 函式呼叫串流跑模擬
#   2. 檢查 .fe / .fe.wp overlay 欄位合法、大小與 block 數相符
#   3. 檢查 TAGE 有學到（誤預測率隨時間下降、最終低於門檻）
#   4. 對照組：純 bimodal 應該明顯比 TAGE 差
#   5. separate（預設）與 inline 兩種 wrong-path 版面都要正確
#   6. 壓力模式：RAS 深度不足與間接跳躍
#   7. .fe.meta.json 的配置指紋要對得上
# ============================================================
set -e
cd "$(dirname "$0")"
B=build
SIM=$B/frontend_sim
CHK=$B/fe_check

[ -x "$SIM" ] || { echo "selftest: 請先 make"; exit 1; }
[ -f "$B/loop.trace" ] || python3 gen_trace.py 400 20 > $B/loop.trace
[ -f "$B/hard.trace" ] || python3 gen_trace.py --hard 2000 > $B/hard.trace

echo "=== [1] TAGE + wrong-path，預設 separate 版面 (config.json) ==="
$SIM $B/loop.trace config.json $B/loop.fe 2> $B/loop.log
cat $B/loop.log

echo
echo "=== [2] 純 bimodal 對照組 (config_bimodal.json) ==="
$SIM $B/loop.trace config_bimodal.json $B/bim.fe 2> $B/bim.log
grep -E "mispredicts|cond direction" $B/bim.log

echo
echo "=== [3] 無 wrong-path 對照組 (config_nowp.json, D=0) ==="
$SIM $B/loop.trace config_nowp.json $B/nowp.fe 2> $B/nowp.log
grep -E "mispredicts|cond direction" $B/nowp.log

echo
echo "=== [4] inline 版面（舊格式，仍支援） ==="
echo '{"wrongpath_out":"inline"}' > $B/inl.json
$SIM $B/loop.trace $B/inl.json $B/inl.fe 2> $B/inl.log
grep -E "fe blocks" $B/inl.log

echo
echo "=== [4b] 輸入格式的三種慣例 ==="
# (a) 6 欄 + not-taken target=0（gen_trace 舊慣例）-> 沿用舊行為
awk '{if($2==1&&$4==0)$5="0"; print $1,$2,$3,$4,$5,$6}' $B/loop.trace > $B/legacy_a.trace
$SIM $B/legacy_a.trace config.json $B/legacy_a.fe 2> $B/legacy_a.log
grep -E "instructions|mispredicts" $B/legacy_a.log | sed 's/^/  (a) /'
# (b) 6 欄 + not-taken target 非 0（Agent H 舊檔）-> 必須報錯
awk '{print $1,$2,$3,$4,$5,$6}' $B/loop.trace > $B/legacy_b.trace
if $SIM $B/legacy_b.trace config.json $B/legacy_b.fe 2> $B/legacy_b.log; then
    echo "  (b) 沒有報錯 —— 不對"; echo "AMBIGUOUS_NOT_REJECTED" > $B/legacy_b.rc
else
    echo "  (b) 正確拒絕，exit=$? "; echo "$?" > $B/legacy_b.rc
fi

echo
echo "=== [5] overlay 合法性檢查 ==="
$CHK $B/loop.fe
$CHK $B/loop.fe.wp | tail -3
$CHK $B/inl.fe | tail -3

echo
echo "=== [6] 壓力模式：深度 20 呼叫鏈（> RAS 16）+ 輪替目標 jalr ==="
$SIM $B/hard.trace config.json $B/hard.fe 2> $B/hard.log
grep -E "mispredicts|RAS|uBTB" $B/hard.log
$CHK $B/hard.fe | tail -3

echo
echo "=== [7] wrong-path 對 uBTB 的 prefetch 效應與 D==容量 懸崖（§10.2）==="
for U in 8 16; do
  for D in $(expr $U - 1) $U; do
    python3 -c "
import json
c=json.load(open('config.json')); c['ubtb_entries']=$U; c['wrongpath_depth']=$D
json.dump(c,open('$B/cliff_u${U}d${D}.json','w'))"
    $SIM $B/hard.trace $B/cliff_u${U}d${D}.json $B/cliff_u${U}d${D}.fe 2>/dev/null
    python3 -c "
import json;s=json.load(open('$B/cliff_u${U}d${D}.stats.json'))
print('  ubtb_entries=%2d  D=%2d  ->  uBTB 命中 %6.2f%%  平均 bubble %.4f' % ($U, $D, 100*s['ubtb_hit_rate'], s['avg_bubbles_per_block']))"
  done
done

echo
echo "=== [8] 斷言 ==="
python3 - <<'PY'
import hashlib, json, os, sys

B = "build"
fail = []

def load(n):
    with open(os.path.join(B, n)) as f:
        return json.load(f)

def size(n):
    return os.path.getsize(os.path.join(B, n))

tage = load("loop.stats.json")
bim  = load("bim.stats.json")
nowp = load("nowp.stats.json")
hard = load("hard.stats.json")
inl  = load("inl.stats.json")
meta = load("loop.fe.meta.json")
D    = 10

# --- 輸入格式相容性 ---
lega = load("legacy_a.stats.json")
if lega["trace_format_columns"] != 6:
    fail.append("legacy_a 應該被判定為 6 欄")
if lega["mispredicts"] != tage["mispredicts"]:
    fail.append("6 欄舊慣例 (a) 的結果應與 7 欄一致：%d vs %d"
                % (lega["mispredicts"], tage["mispredicts"]))
if tage["trace_format_columns"] != 7:
    fail.append("主路徑應該吃到 7 欄格式")
rc = open(os.path.join(B, "legacy_b.rc")).read().strip()
if rc == "AMBIGUOUS_NOT_REJECTED":
    fail.append("6 欄 + not-taken target 非 0 的歧義檔案應該被拒絕")

# --- 保守預設：wrong path 不訓練 TAGE 計數器（監督者裁決）---
cfg = json.load(open("config.json"))
if cfg.get("wrongpath_train_tage", 1) != 0:
    fail.append("config.json 的 wrongpath_train_tage 應該是 0")
if cfg.get("wrongpath_train_btb", 0) != 1:
    fail.append("config.json 的 wrongpath_train_btb 應該是 1")
if meta.get("bstf_version") != 2:
    fail.append("meta 的 bstf_version 應該是 2（實際 %s）" % meta.get("bstf_version"))

# --- separate（預設）：.fe 純 correct-path、線性；wrong-path 在 .fe.wp ---
if meta["wrongpath_out"] != "separate":
    fail.append("預設不是 separate 版面（實際 %s）" % meta["wrongpath_out"])
if size("loop.fe") != tage["correct_path_blocks"]:
    fail.append(".fe %d byte != correct_path_blocks %d（預設版面必須是純 correct path）"
                % (size("loop.fe"), tage["correct_path_blocks"]))
if tage["total_fe_blocks"] != tage["correct_path_blocks"]:
    fail.append("separate 版面下 total_fe_blocks 應該等於 correct_path_blocks")
if size("loop.fe.wp") != tage["wrongpath_blocks"]:
    fail.append(".fe.wp %d byte != wrongpath_blocks %d" % (size("loop.fe.wp"), tage["wrongpath_blocks"]))
if tage["wrongpath_blocks"] != tage["mispredicts"] * D:
    fail.append(".fe.wp 不是每次誤預測 D 個 byte：%d != %d*%d"
                % (tage["wrongpath_blocks"], tage["mispredicts"], D))

# --- inline 版面：total = correct + wrong，且不改變 correct-path 結果 ---
if inl["total_fe_blocks"] != inl["correct_path_blocks"] + inl["wrongpath_blocks"]:
    fail.append("inline 版面 total_fe_blocks 不等於 correct + wrong")
if size("inl.fe") != inl["total_fe_blocks"]:
    fail.append("inline .fe 大小與 total_fe_blocks 不符")
if inl["mispredicts"] != tage["mispredicts"] or inl["correct_path_blocks"] != tage["correct_path_blocks"]:
    fail.append("版面設定改變了 correct-path 結果")

# --- meta sidecar 的指紋 ---
h = hashlib.sha256(open("config.json", "rb").read()).hexdigest()
if meta["config_sha256"] != h:
    fail.append("meta 的 config_sha256 對不上 config.json")
if meta["wrongpath_depth"] != D or meta["wrongpath_bytes"] != size("loop.fe.wp"):
    fail.append("meta 的 wrongpath 欄位與實際檔案不符")
if meta["n_fe_blocks"] != tage["correct_path_blocks"] or meta["fe_bytes"] != size("loop.fe"):
    fail.append("meta 的 fe 欄位與實際檔案不符")
if len(meta.get("effective_config_sha256", "")) != 64:
    fail.append("meta 缺少有效配置指紋")

# --- correct-path block 數在各配置間必須一致 ---
if not (tage["correct_path_blocks"] == bim["correct_path_blocks"] == nowp["correct_path_blocks"]):
    fail.append("correct_path_blocks 不一致")
if tage["wrongpath_blocks"] <= 0:
    fail.append("wrongpath_depth=10 卻沒有 wrong-path block")
if nowp["wrongpath_blocks"] != 0:
    fail.append("wrongpath_depth=0 卻產生了 wrong-path block")

# --- TAGE 要學到：最後一個十分位 << 第一個十分位 ---
dec = tage["mispredict_rate_by_decile"]
if dec[-1] >= dec[0]:
    fail.append("誤預測率沒有下降: %.4f -> %.4f" % (dec[0], dec[-1]))
if dec[-1] > 0.02:
    fail.append("最終誤預測率 %.4f 太高（門檻 0.02）" % dec[-1])

# --- TAGE 必須明顯優於 bimodal ---
if tage["cond_dir_mispredict_rate"] >= bim["cond_dir_mispredict_rate"] * 0.5:
    fail.append("TAGE 沒有明顯優於 bimodal: %.4f vs %.4f"
                % (tage["cond_dir_mispredict_rate"], bim["cond_dir_mispredict_rate"]))

# --- RAS ---
if tage["ras_predictions"] == 0:
    fail.append("串流裡沒有 ret？")
elif tage["ras_accuracy"] < 0.99:
    fail.append("RAS 準確率只有 %.4f" % tage["ras_accuracy"])
if not (0.70 <= hard["ras_accuracy"] <= 0.90):
    fail.append("壓力模式 RAS 準確率 %.4f 不在預期的 16/20 附近" % hard["ras_accuracy"])
if not (0.05 <= hard["mispredict_rate"] <= 0.30):
    fail.append("壓力模式誤預測率 %.4f 不合理" % hard["mispredict_rate"])
if hard["wrongpath_blocks"] != hard["mispredicts"] * D:
    fail.append("壓力模式 wrong-path block 數不等於誤預測數 * D")

# --- §10.2 懸崖：D = U-1 有 prefetch 效應、D = U 歸零（U = uBTB entry 數）---
for U in (8, 16):
    lo = load("cliff_u%dd%d.stats.json" % (U, U - 1))["ubtb_hit_rate"]
    hi = load("cliff_u%dd%d.stats.json" % (U, U))["ubtb_hit_rate"]
    if lo < 0.10:
        fail.append("U=%d, D=%d 的 uBTB 命中率 %.4f 太低（應有 wrong-path prefetch 效應）" % (U, U-1, lo))
    if hi > 0.01:
        fail.append("U=%d, D=%d 的 uBTB 命中率 %.4f 應該歸零（D==容量 懸崖）" % (U, U, hi))

# --- 保守預設下誤預測率不應該隨 D 改變 ---
if tage["mispredicts"] != nowp["mispredicts"]:
    fail.append("保守預設下 D=10 (%d) 與 D=0 (%d) 的誤預測數應該相同"
                % (tage["mispredicts"], nowp["mispredicts"]))

if tage["avg_bubbles_per_block"] > 1.0:
    fail.append("平均 bubble/block = %.3f 太高" % tage["avg_bubbles_per_block"])

print("decile mispredict rate : %s" % " ".join("%.4f" % d for d in dec))
print("TAGE  cond dir mispred : %.5f   (MPKI %.3f)" % (tage["cond_dir_mispredict_rate"], tage["mpki"]))
print("bimod cond dir mispred : %.5f   (MPKI %.3f)" % (bim["cond_dir_mispredict_rate"], bim["mpki"]))
print("uBTB hit / BTB override: %.4f / %.4f" % (tage["ubtb_hit_rate"], tage["btb_override_rate"]))
print("avg bubbles per block  : %.4f" % tage["avg_bubbles_per_block"])
print("RAS accuracy           : %.4f" % tage["ras_accuracy"])
print("格式相容性           : 7 欄 %d 誤預測 == 6 欄(a) %d 誤預測；6 欄(b) 已拒絕"
      % (tage["mispredicts"], lega["mispredicts"]))
print(".fe / .fe.wp bytes     : %d / %d   (D=%d, 誤預測 %d 次)"
      % (size("loop.fe"), size("loop.fe.wp"), D, tage["mispredicts"]))
print("D 不敏感性確認     : D=10 誤預測 %d == D=0 誤預測 %d" % (tage["mispredicts"], nowp["mispredicts"]))
print("懸崖 (hard.trace)      : U=16 D=15 uBTB %.4f -> D=16 uBTB %.4f"
      % (load("cliff_u16d15.stats.json")["ubtb_hit_rate"],
         load("cliff_u16d16.stats.json")["ubtb_hit_rate"]))
print("hard: mispred %.4f  RAS %.4f  uBTB %.4f  bubbles %.4f"
      % (hard["mispredict_rate"], hard["ras_accuracy"], hard["ubtb_hit_rate"],
         hard["avg_bubbles_per_block"]))

if fail:
    print("\nSELFTEST FAILED:")
    for m in fail:
        print("  -", m)
    sys.exit(1)
print("\nSELFTEST PASSED")
PY
