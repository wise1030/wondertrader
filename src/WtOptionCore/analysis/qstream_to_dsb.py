#!/usr/bin/env python3
"""P1 follow-up (6.5): qstream CSV -> WT dsb tick converter.

Converts a recorded quote stream (generated/qstream_<tdate>.csv, written by
QstreamRecorder) into WonderTrader's native dsb tick blocks that
WtBtDtReader/WtBtRunner read directly:

    his/ticks/<EXCHG>/<tdate>/<code>.dsb

Each output file is one BlockHeaderV2{type=BT_HIS_Ticks(24),
version=BLOCK_VERSION_RAW_V2(0x03)} followed by N packed WTSTickStruct
records (pack(8), 832 bytes each — see Includes/WTSStruct.h).  RAW (no
compression) is deliberate: proc_block_data passes non-compressed V2
blocks through unchanged, and the replayer only needs read support.

The qstream captures (ts, code, bid, ask, last, iv) — fields the option
strategy's full chain (on_tick -> grid -> computeValues -> CTG) needs to
rebuild OptionMarket.  Fields the strategy does not consume (open/high/
low, turnover, interest) stay 0, matching a fresh-session replay.

IMPORTANT: qstream timestamps are STRATEGY-CLOCK seconds (not exchange
HHMMSS).  The replayer orders ticks by action_time; we map
ts -> action_time as HHMMSSmmm from the ts's UTC clock so intraday
ordering is preserved.

Usage:
    python3 qstream_to_dsb.py <qstream.csv> <out_dir> <exchg_upper>
    # e.g. python3 qstream_to_dsb.py generated/qstream_20261120.csv /btdata SHFE
"""
import csv
import os
import struct
import sys
import time
from collections import defaultdict

TICK_STRUCT = struct.Struct(
    "<16s32s"          # exchg, code
    + "d" * 13         # price, open, high, low, settle, upper, lower,
                       # total_volume, volume, total_turnover, turnover,
                       # open_interest, diff_interest
    + "I" * 4          # trading_date, action_date, action_time, reserve
    + "d" * 3          # pre_close, pre_settle, pre_interest
    + "d" * 40         # bid_prices[10], ask_prices[10], bid_qty[10], ask_qty[10]
)
TICK_SIZE = TICK_STRUCT.size   # 512 bytes under pack(8)
BLK_FLAG = b"&^%$#@!\x00"
BT_HIS_TICKS = 24
BLOCK_VERSION_RAW_V2 = 0x03
# RAW blocks use the 12-byte legacy BlockHeader (char[8]+uint16+uint16) — the
# reader's RAW pass-through erases BLOCK_HEADER_SIZE(12), NOT the V2 20-byte
# header (which is only meaningful for compressed blocks, where the size
# field is needed for decompression).  A 20-byte header on a RAW block would
# leave 8 stray bytes in the payload and misalign every tick struct.
HEADER = struct.Struct("<8sHH")


def action_time_from_ts(ts: float) -> int:
    """strategy-clock seconds -> HHMMSSmmm (localtime of that epoch)."""
    t = time.localtime(ts)
    return (t.tm_hour * 10000 + t.tm_min * 100 + t.tm_sec) * 1000 + int(
        (ts - int(ts)) * 1000)


def main() -> int:
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    qpath, out_root, exchg = sys.argv[1], sys.argv[2], sys.argv[3].upper()

    by_code_day: dict[tuple[str, int], list[tuple[float, float, float, double, double]]] \
        = defaultdict(list)
    with open(qpath, newline="") as fh:
        for row in csv.DictReader(fh):
            try:
                ts = float(row["ts"])
                code = row["code"]
                bid, ask, last, iv = (float(row[k]) for k in ("bid", "ask", "last", "iv"))
            except (KeyError, TypeError, ValueError):
                continue
            if bid <= 0 or ask <= 0:
                continue   # no book -> not replayable as a market tick
            day = 0        # grouping key only; the real tdate comes from the filename
            by_code_day[(code, day)].append((ts, bid, ask, last))

    # tdate comes from the filename: qstream_<tdate>.csv
    tdate = os.path.basename(qpath).replace("qstream_", "").replace(".csv", "")
    if not tdate.isdigit():
        print(f"cannot derive tdate from {qpath}")
        return 2

    n_files = n_ticks = 0
    for (code, _), rows in by_code_day.items():
        rows.sort()
        out_dir = os.path.join(out_root, "his", "ticks", exchg, tdate)
        os.makedirs(out_dir, exist_ok=True)
        # WtBtDtReader addresses his/ticks/<exchg>/<date>/<bare_code>.dsb —
        # qstream records stdCodes ("SHFE.cu2607C..."), strip the prefix.
        bare = code.split(".", 1)[1] if "." in code else code
        payload = bytearray()
        prev_vol = 0
        for ts, bid, ask, last in rows:
            at = action_time_from_ts(ts)
            ad = int(time.strftime("%Y%m%d", time.localtime(ts)))
            # volume = cumulative trade count delta approximated by row count
            prev_vol += 1
            rec = TICK_STRUCT.pack(
                exchg.encode()[:15], bare.encode()[:31],
                last, 0.0, 0.0, 0.0, 0.0,   # price, open, high, low, settle
                0.0, 0.0,                    # upper/lower (limits live in cfg)
                float(prev_vol), 1.0, 0.0, 0.0, 0.0, 0.0,
                int(tdate), ad, at, 0,
                0.0, 0.0, 0.0,
                *([bid] + [0.0] * 9 + [ask] + [0.0] * 9
                  + [1.0] + [0.0] * 9 + [1.0] + [0.0] * 9),
            )
            payload += rec
        header = HEADER.pack(BLK_FLAG, BT_HIS_TICKS, BLOCK_VERSION_RAW_V2)
        out_path = os.path.join(out_dir, f"{bare}.dsb")
        with open(out_path, "wb") as out:
            out.write(header)
            out.write(payload)
        n_files += 1
        n_ticks += len(rows)

    print(f"wrote {n_files} dsb files, {n_ticks} ticks, tdate={tdate}, exchg={exchg}")
    print("next: point WtBtRunner's backtest config at this data dir "
          "(see src/WtOptionCore/analysis/replay.md)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
