#!/usr/bin/env bash
# 端到端期权tick replay: KS缓存 -> TickConverter -> DSB -> WtBtRunner -> p99/p99.9
# 用法: ./run_option_replay.sh [DATES_CSV]   默认 20260602,20260603,20260604,20260605
set -euo pipefail

DATES="${1:-20260602,20260603,20260604,20260605}"
REPO=/mnt/d/gf_pc/WonderTrader/wondertrader
KS2WTPY=/mnt/d/gf_pc/WonderTrader/wtpy/tools/ks2wtpy
CACHE=/tmp/opencode/ks_s3cache
STORAGE="$REPO/dist/WtBtFutu/storage"
RUNNER_DIR="$REPO/dist/WtBtRunner"

cd "$KS2WTPY"
echo "=== [1/3] tick -> DSB (dates=$DATES) ==="
python3 main.py --tick --s3 \
  --s3-endpoint 172.16.36.3:6900 \
  --s3-access-key "$(python3 -c "import json;print(json.load(open('credentials.json'))['accessKey'])")" \
  --s3-secret-key "$(python3 -c "import json;print(json.load(open('credentials.json'))['secretKey'])")" \
  --s3-bucket ks-data \
  --s3-cache-dir "$CACHE" \
  --products SHFE.cu \
  --include-option \
  --tick-dates "$DATES" \
  --output "$STORAGE" \
  --log-level INFO

echo "=== [2/3] verify option DSBs ==="
total_opt=0
for d in ${DATES//,/ }; do
  n=$(ls "$STORAGE/his/ticks/SHFE/$d" 2>/dev/null | grep -c -E '[0-9](C|P)[0-9]{4}\.dsb' || true)
  echo "  $d: $n option DSB files"
  total_opt=$((total_opt + n))
done
[ "$total_opt" -gt 0 ] || { echo "ERROR: no option tick DSBs produced"; exit 1; }

echo "=== [3/3] WtBtRunner replay ==="
cd "$RUNNER_DIR"
cp "$REPO/src/WtOptionCore/replay/configbt_option_replay.yaml" .
env LD_LIBRARY_PATH="$REPO/src/build_all/build_x64/Debug/bin" \
  ./WtBtRunner.fresh -c configbt_option_replay.yaml -l logcfgbt_option.yaml

echo "=== batch latency (p99/p99.9 us) ==="
grep -h -o 'p99=[0-9.]*us p99.9=[0-9.]*us' Logs/Strategy/*.log | sort | uniq -c | tail -20 || true
