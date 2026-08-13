#!/usr/bin/env python3
"""Convert UFT engine trade.membin (binary) to trades.csv for MonSvr display.

UFT engine outputs trade.membin (the strategy's own position journal) instead of
CSV. This script parses trade.membin directly and writes the monitor's
trades.csv. Every nonzero-volume record maps to one row; one fill may
legitimately split into close+open records (this is the strategy's own book view
and reflects the real position transitions). Zero-volume records are position
accounting artifacts and are skipped.

Record layout (see src/WtFutuCore/../WtUftCore/UftDataDefs.h):
  header 24B: magic(8) + type(4) + date(4) + capacity(4) + size(4)
  record 84B: exchg(16) + code(32) + direct(4) + offset(4) + volume(8)
              + price(8) + trading_date(4) + trading_time(8), pack(1)
  direct: 0=long(buy side), 1=short(sell side); offset: 0=open, 1=close
"""
import os
import struct
import sys
from datetime import datetime, timezone, timedelta

HEADER = struct.Struct("<8sIIII")
RECORD = struct.Struct("<16s32sIIddIQ")

CST = timezone(timedelta(hours=8))
DIRECT_MAP = {0: "BUY", 1: "SELL"}
OFFSET_MAP = {0: "OPEN", 1: "CLOSE"}


def parse_membin(path):
    trades = []
    with open(path, "rb") as f:
        raw = f.read(HEADER.size)
        if len(raw) < HEADER.size:
            return trades
        magic, typ, date, capacity, size = HEADER.unpack(raw)
        for _ in range(size):
            raw = f.read(RECORD.size)
            if len(raw) < RECORD.size:
                break
            exchg, code, direct, offset, volume, price, tdate, ttime = RECORD.unpack(raw)
            if volume == 0.0:
                continue
            exchg_s = exchg.split(b"\x00")[0].decode(errors="replace")
            code_s = code.split(b"\x00")[0].decode(errors="replace")
            full_code = f"{exchg_s}.{code_s}" if exchg_s else code_s
            dt = datetime.fromtimestamp(ttime / 1000.0, tz=CST)
            time_int = int(dt.strftime("%Y%m%d%H%M%S"))
            direct_s = DIRECT_MAP.get(direct, "SELL" if direct else "BUY")
            offset_s = OFFSET_MAP.get(offset, "CLOSE" if offset else "OPEN")
            trades.append((full_code, time_int, direct_s, offset_s, price, volume))
    return trades


def write_csv(trades, csv_path):
    os.makedirs(os.path.dirname(csv_path), exist_ok=True)
    with open(csv_path, "w") as f:
        f.write("code,time,direct,action,price,qty,tag,fee,barno\n")
        for code, t, direct, offset, price, qty in trades:
            f.write(f"{code},{t},{direct},{offset},{price},{qty},,0,0\n")


def main():
    dist_dir = os.path.dirname(os.path.abspath(__file__))
    if len(sys.argv) > 1:
        membin_path = sys.argv[1]
    else:
        membin_path = os.path.join(dist_dir, "generated", "outputs",
                                   "futu_mm_ao_live", "trade.membin")
    if len(sys.argv) > 2:
        csv_path = sys.argv[2]
    else:
        csv_path = os.path.join(dist_dir, "generated", "outputs",
                                "futu_mm_ao_live", "trades.csv")
    trades = parse_membin(membin_path)
    write_csv(trades, csv_path)
    print(f"Converted {len(trades)} trades from {membin_path} -> {csv_path}")


if __name__ == "__main__":
    main()
