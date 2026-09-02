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
# V10-R9c: wtpy MonSvr 前端按 direction→多/空 + offset→开/平 组合渲染
# ("OPEN"==offset?"开":"平"; "LONG"==direction?"多":"空")。
# direction 必须填成交作用的持仓域: OPEN 按成交方向 (买开=LONG/卖开=SHORT),
# CLOSE 取反 (买平平的是空头→SHORT/卖平平的是多头→LONG) —— 与 CTP 下发 offset
# 语义一致 (买开=开多/卖开=开空/买平=平空/卖平=平多)。
# 旧实现直接输出成交方向 BUY/SELL: 前端 "LONG" 恒不匹配 → 全部显示为 开空/平空。
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
            if offset == 0:
                direct_s = "LONG" if direct == 0 else "SHORT"
            else:
                direct_s = "SHORT" if direct == 0 else "LONG"
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
