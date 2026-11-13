#!/bin/bash
# golden_extract.sh LOG MODE [TRADES_CSV]
#
# Emits the NORMALIZED STRUCTURAL SET of a replay run for golden-baseline
# comparison (audit 2026-11-13 v2, batch A).  Two tiers, after write-run
# evidence forced a downgrade of the original third tier:
#   structural  — payload/sim-clock driven, must match run-to-run exactly
#                 (B4 stop/resume, flatten ARM lines, session lifecycle tick
#                 counts, readiness session-open BLOCK family, queue-integrity
#                 and QuoteDuty skeletons)
#   molecular   — normalized to N.  NOTE the deeper finding: fills are
#                 molecular even under error_rate=0 — the async worker
#                 places orders at the SIM time reached when the engine
#                 thread happens to process them, so order timestamps leak
#                 engine/worker real-time interleaving.  The original
#                 "full-equality (norand)" tier (futures fill set + combo
#                 outcomes) was dropped on exactly this evidence.
#
# MODE is kept for future tier splits; both registered configs currently
# extract the same structural set.
set -e
LOG=$1; MODE=${2:-default}; TRADES=${3:-}; CSV=${4:-}
[ -f "$LOG" ] || { echo "golden_extract: no log $LOG" >&2; exit 2; }
case "$MODE" in default|norand) ;; *) echo "golden_extract: bad MODE $MODE" >&2; exit 2;; esac

strip() { sed 's/^[^]]*] //'; }

# --- scheduling actions (payload clock — exact) --------------------------
grep -E "Session (stop|resume)" "$LOG" | strip
# Close-flatten ARM lines only: the arm transitions are time-pure (always
# exactly 2 per day x sessions).  DISARM lines are RNG-conditioned (hedge
# in-flight state at the close window varies run-to-run and can add an
# extra disarm transition) — golden gate write-run evidence 2026-11-13.
grep "Close flatten armed" "$LOG" | strip
# --- session lifecycle (tick count is feed-driven — exact) ---------------
# total_ticks lines only; the "non-zero gross position" companion line is
# RNG-conditioned in both presence and size — excluded.
grep "session_end:" "$LOG" | strip | grep "total_ticks"
# --- market-readiness gating ---------------------------------------------
# "session open" BLOCK family only (per-session gating is structural); the
# "insufficient valid market points" flavor is fit-conditioned and observed
# to vary run-to-run even under error_rate=0 — excluded.
grep "MarketReadiness" "$LOG" | strip \
    | grep "session open" \
    | sed -E 's/expiry [0-9]+ /expiry N /'
# --- queue integrity ------------------------------------------------------
# From the CSV sidecar (deterministic, survives the exit race) when present;
# falls back to the log line otherwise.  dropsDelta is structural (0);
# counts/latency are molecular — normalized to N.
if [ -n "$CSV" ] && [ -f "$CSV" ]; then
    awk -F, 'NR>1 {print "AEP session "$1": dropsDelta="$3" mdOverflowDepth=N traderQueueDepth=N"}' "$CSV"
else
    grep "AEP session" "$LOG" | strip \
        | sed -E 's/events=[0-9]+/events=N/; s/p50=[0-9.]+us p95=[0-9.]+us/p50=Nus p95=Nus/'
fi
# --- quoting duty (molecular numerator/window, structural shape) ----------
grep "QuoteDuty session" "$LOG" | strip \
    | sed -E 's/enabled [0-9.]+s of [0-9.]+s/enabled Ns of Ns/; s/\([0-9.]+%\)/(N%)/'
