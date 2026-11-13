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
# Sync the working copies under version control in src/ — on drift the dist
# copy is REFRESHED from src/ (it is a disposable working copy, not a source
# of truth) and the run continues with a warning.
YAML="${YAML:-configbt_option_replay.yaml}"   # e.g. YAML=configbt_option_replay_attr.yaml
# Fresh logs for a fresh run: the gate below must analyze THIS run only, so
# clear previous Runner logs (they are reproducible from the replay).  Same
# for the AEP metrics CSV sidecar (it is a per-run ledger, not a history).
rm -f Logs/Runner_*.log outputs_bt/aep_metrics.csv
for f in "$YAML"; do
  if ! cmp -s "$SCRIPT_DIR/$f" "$f"; then
    echo "NOTE: $f differs from src/ copy — refreshing"
    cp "$SCRIPT_DIR/$f" .
  fi
done
if [ -d replay_common ]; then
  diff -r -q "$SCRIPT_DIR/replay_common" replay_common >/dev/null 2>&1 || {
    echo "NOTE: replay_common differs from src/ copy — refreshing"
    rm -rf replay_common && cp -r "$SCRIPT_DIR/replay_common" .
  }
else
  cp -r "$SCRIPT_DIR/replay_common" .
fi
# ABI mismatch between the strategy module and the runner is a real failure
# mode (segfault / bogus FAIL), and the dist copy is refreshed from the build
# output by hand today — same drift-refresh rule as the yaml above.
SRC_SO="$REPO/src/build_all/WtOptionCore/libWtOptionCore.so"
DST_SO="hft/libWtOptionCore.so"
if [ -f "$SRC_SO" ] && ! cmp -s "$SRC_SO" "$DST_SO"; then
  echo "NOTE: libWtOptionCore.so differs from build output — refreshing"
  cp "$SRC_SO" "$DST_SO"
fi
env LD_LIBRARY_PATH="$REPO/src/build_all/build_x64/Debug/bin" \
  ./WtBtRunner.fresh -c "$YAML" -l logcfgbt_option.yaml

echo "=== batch latency (p99/p99.9 us) ==="
grep -h -o 'p99=[0-9.]*us p99.9=[0-9.]*us' Logs/Strategy/*.log | sort | uniq -c | tail -20 || true

# === [4/4] runtime error gate ============================================
# exit=0 only proves the runner did not crash — it says NOTHING about runtime
# errors.  Count them with a regex that matches the actual log format
# ("[ts - error]"; note NO trailing space after the level).  Categories:
#   HARD  — attribution/anomaly integrity: fail the script (unless --no-strict)
#   SOFT  — risk-path events: listed for triage, allowed via ALLOW_PATTERNS
LOG_F=$(ls -t Logs/Runner_*.log 2>/dev/null | head -1)
if [ -n "$LOG_F" ]; then
  # NOTE: refresh-on-drift (below) is intentional — the dist copy is a
  # disposable working copy, src/ stays the single source of truth.
  TOTAL_ERR=$(grep -cE ' - (error|fatal)\]' "$LOG_F" || true)
  hard() { grep -E ' - (error|fatal)\]' "$LOG_F" | grep -E 'order-not-found fill|OVERFILL|fill for REJECTED order|ORDER ANOMALY ESCALATION' || true; }
  HARD_N=$(hard | grep -c . || true)
  echo "=== runtime error gate: $LOG_F (total=$TOTAL_ERR hard=$HARD_N) ==="
  if [ "$HARD_N" -gt 0 ]; then
    echo "HARD failures (attribution/anomaly integrity):"
    hard | sed 's/^/  /' | head -20
    [ "${STRICT:-1}" = "1" ] && { echo "GATE FAILED (set STRICT=0 to override)"; exit 1; }
  fi
  # SOFT triage summary (risk-path events — review, don't auto-fail)
  grep -E ' - (error|fatal)\]' "$LOG_F" \
    | grep -vE 'order-not-found fill|OVERFILL|fill for REJECTED order|ORDER ANOMALY ESCALATION' \
    | sed -E 's/^\[[^]]*\] //' \
    | sed -E 's/[0-9]+\.?[0-9]*//g' \
    | sort | uniq -c | sort -rn | head -15 \
    | sed 's/^/  SOFT /' || true
else
  echo "WARNING: no runner log found — gate skipped"
fi
