#!/usr/bin/env bash
# 端到端期权tick replay: 取数(缺数据时) -> DSB校验 -> WtBtRunner -> p99/p99.9
# 用法: ./run_option_replay.sh [DATES_CSV]   默认 20260602,20260603,20260604,20260605
set -euo pipefail

DATES="${1:-20260602,20260603,20260604,20260605}"
CHAIN=cu2607
REPO=/mnt/d/gf_pc/WonderTrader/wondertrader
STORAGE="$REPO/dist/WtBtFutu/storage"
RUNNER_DIR="$REPO/dist/WtBtRunner"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "=== [1/3] tick -> DSB (dates=$DATES, chain=$CHAIN) ==="
need_fetch=0
for d in ${DATES//,/ }; do
  n=$(ls "$STORAGE/his/ticks/SHFE/$d" 2>/dev/null | grep -c -E "^${CHAIN}[CP]" || true)
  [ "$n" -gt 0 ] || need_fetch=1
done
if [ "$need_fetch" -eq 1 ]; then
  echo "local option DSBs missing, fetching via server-side pipeline..."
  bash "$SCRIPT_DIR/fetch_option_ticks.sh" cu "$CHAIN" "$DATES"
else
  echo "local option DSBs already present, skip fetch"
fi

echo "=== [2/3] verify option DSBs ==="
total_opt=0
for d in ${DATES//,/ }; do
  n=$(ls "$STORAGE/his/ticks/SHFE/$d" 2>/dev/null | grep -c -E "^${CHAIN}[CP]" || true)
  echo "  $d: $n option DSB files"
  total_opt=$((total_opt + n))
done
[ "$total_opt" -gt 0 ] || { echo "ERROR: no option tick DSBs produced"; exit 1; }

echo "=== [3/3] WtBtRunner replay ==="
cd "$RUNNER_DIR"
cp "$SCRIPT_DIR/configbt_option_replay.yaml" .
rm -rf replay_common && cp -r "$SCRIPT_DIR/replay_common" .
env LD_LIBRARY_PATH="$REPO/src/build_all/build_x64/Debug/bin" \
  ./WtBtRunner.fresh -c configbt_option_replay.yaml -l logcfgbt_option.yaml

echo "=== batch latency (p99/p99.9 us) ==="
grep -h -o 'p99=[0-9.]*us p99.9=[0-9.]*us' Logs/Strategy/*.log | sort | uniq -c | tail -20 || true
