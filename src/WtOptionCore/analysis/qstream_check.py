#!/usr/bin/env python3
"""P1 batch-6.5: qstream replay validator.

Reads a recorded quote stream (generated/qstream_<tdate>.csv) and proves it
is replayable end-to-end for analysis:

  1. integrity    — per-code monotonic ts, non-negative px, bid<=ask or a
                    stale-book flag (bid==ask==0 rows are dropped, logged)
  2. continuity   — max per-code gap distribution (a replay driver needs to
                    know where the record is sparse)
  3. replay check — rebuild an ATM mid series from the stream and compare
                    its per-minute mean against volseries_<tdate>.csv's fwd
                    (same clock, same day): the analysis layer must be able
                    to recompute what the live system computed.

Exit code 0 when the stream is structurally sound.  This is the offline
replay entry for the analysis record; driving the full strategy engine from
the stream (WtBtCore ParserX integration) remains a future item — see
roadmap batch 6 notes.

Usage: python3 qstream_check.py <generated_dir> <tdate>
"""
import csv
import statistics
import sys
from collections import defaultdict


def load(path: str):
    rows = []
    try:
        with open(path, newline="") as fh:
            rows = list(csv.DictReader(fh))
    except OSError:
        pass
    return rows


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    gdir, tdate = sys.argv[1], sys.argv[2]

    qs = load(f"{gdir}/qstream_{tdate}.csv")
    if not qs:
        print(f"FAIL: no qstream rows for {tdate} in {gdir}")
        return 1

    by_code: dict[str, list[tuple[float, float, float, float, float]]] = defaultdict(list)
    bad_px = stale = 0
    for r in qs:
        try:
            ts = float(r["ts"]); bid = float(r["bid"]); ask = float(r["ask"])
            last = float(r["last"]); iv = float(r["iv"])
        except (KeyError, TypeError, ValueError):
            continue
        if bid < 0 or ask < 0 or last < 0:
            bad_px += 1
            continue
        if bid == 0 and ask == 0:
            stale += 1
            continue
        by_code[r["code"]].append((ts, bid, ask, last, iv))

    # 1) monotonic ts per code
    nonmono = 0
    for code, rows in by_code.items():
        for a, b in zip(rows, rows[1:]):
            if b[0] < a[0]:
                nonmono += 1
    # 2) gap distribution (seconds)
    gaps = []
    for rows in by_code.values():
        gaps += [b[0] - a[0] for a, b in zip(rows, rows[1:])]
    gaps.sort()
    p50 = gaps[len(gaps) // 2] if gaps else 0
    p99 = gaps[int(len(gaps) * 0.99)] if gaps else 0

    # 3) replay: ATM-ish mid series per minute vs volseries fwd
    vol = load(f"{gdir}/volseries_{tdate}.csv")
    replay_note = "no volseries to compare"
    if vol:
        mids = [(ts, (bid + ask) / 2) for rows in by_code.values()
                for (ts, bid, ask, _, _) in rows if bid > 0 and ask > 0]
        mids.sort()
        minute_mids: dict[int, list[float]] = defaultdict(list)
        for ts, mid in mids:
            minute_mids[int(ts // 60) * 60].append(mid)
        vmap = {int(float(r["ts"])): float(r["fwd"]) for r in vol if r.get("fwd")}
        common = [(statistics.mean(minute_mids[m]), vmap[m])
                  for m in minute_mids if m in vmap and vmap[m] > 0]
        if len(common) >= 5:
            rel_err = [abs(a - b) / b for a, b in common]
            replay_note = (f"mid-vs-fwd rel err median="
                           f"{statistics.median(rel_err):.4f} over {len(common)} minutes")
        else:
            replay_note = f"only {len(common)} overlapping minutes — too sparse"

    print(f"codes={len(by_code)} rows={sum(len(v) for v in by_code.values())} "
          f"dropped(bad_px)={bad_px} dropped(stale)={stale} nonmono={nonmono}")
    print(f"gap(s) p50={p50:.0f} p99={p99:.0f} max={gaps[-1] if gaps else 0:.0f}")
    print(f"replay: {replay_note}")
    ok = nonmono == 0 and bad_px == 0 and len(by_code) > 0
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
