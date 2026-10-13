#!/usr/bin/env python3
"""Journal 对账: trade.membin(策略簿记) vs Runner 日志成交回报 (19.26 F3①/F4).

每分钟由 convert_trades 同族 cron 调用不如日终/盘后跑一次有意义 —— 本脚本设计为
收盘后(cron 15:10 / 01:10)对账当日交易日:

  1. 日志侧: 统计 Runner 日志中 "Trade notified" 笔数与 trdqty 总量
     (含跨午夜夜盘, 按 'hour>=20 则交易日+1' 归属)。
  2. 簿记侧: 解析 trade.membin header size + 记录, 按 trading_date 分组,
     取最新交易日统计行数/量。
  3. 判定:
     - rows_journal >= fills_log            (split 机制: 一腿平仓+开仓拆两行)
     - |vol_journal - vol_log| / vol_log <= 5%   否则报 DOUBLE-WRITE 嫌疑 (19.26 F4)
  4. 输出一行对账记录到 stdout (cron 重定向 reconcile.log)。

用法: reconcile_journals.py <runner_log> [runner_log2 ...] [membin_path]
       (cron 固定传"昨天+今天"两份 Runner 日志: 交易日=夜盘(昨21:00-今01:00)+日盘,
        两个时段分属两天的日志文件, 缺一份必误报 MISMATCH)
"""

import re
import struct
import sys
from collections import defaultdict

TRE = re.compile(r"\[(\d+)\.(\d+)\.(\d+) (\d+):(\d+):(\d+) - \w+\] \[trader_ctp\] Trade notified, instrument: (\S+), usertag: (\S+), trdqty: (\d+), trdprice: ([\d.]+)")
HDR = struct.Struct("<8sIIII")
REC = struct.Struct("<16s32sIIddIQ")


def parse_log(paths):
    by_td = defaultdict(lambda: [0, 0])
    for path in paths:
        try:
            f = open(path, errors="replace")
        except OSError:
            continue
        with f:
            for line in f:
                m = TRE.search(line)
                if not m:
                    continue
                y, mo, d, h = int(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(4))
                td = d + 1 if h >= 20 else d  # 夜盘归属下一交易日
                td_key = y * 10000 + mo * 100 + td
                a = by_td[td_key]
                a[0] += 1
                a[1] += int(m.group(9))
    return by_td


def parse_membin(path):
    by_td = defaultdict(lambda: [0, 0])
    with open(path, "rb") as f:
        magic, typ, hdate, cap, size = HDR.unpack(f.read(HDR.size))
        for _ in range(size):
            raw = f.read(REC.size)
            if len(raw) < REC.size:
                break
            exchg, code, direct, offset, vol, price, tdate, ttime = REC.unpack(raw)
            if vol == 0:
                continue
            a = by_td[tdate]
            a[0] += 1
            a[1] += int(vol)
    return by_td


def main():
    argv = sys.argv[1:]
    membin_path = "generated/outputs/futu_mm_ao_live/trade.membin"
    if argv and argv[-1].endswith(".membin"):
        membin_path = argv[-1]
        argv = argv[:-1]
    log_paths = argv
    if not log_paths:
        print("RECONCILE ERROR no runner logs given")
        return 1

    log_side = parse_log(log_paths)
    jnl_side = parse_membin(membin_path)
    if not jnl_side:
        print(f"RECONCILE ERROR no journal records in {membin_path}")
        return 1

    latest_td = max(jnl_side)
    j_rows, j_vol = jnl_side[latest_td]
    l_fills, l_vol = log_side.get(latest_td, [0, 0])

    status = "OK"
    notes = []
    if j_rows < l_fills:
        status = "MISMATCH"
        notes.append(f"journal rows {j_rows} < log fills {l_fills} (dropped writes?)")
    if l_vol > 0:
        dev = abs(j_vol - l_vol) / l_vol
        if dev > 0.05:
            status = "MISMATCH"
            notes.append(f"volume deviation {dev * 100:.1f}% (journal {j_vol} vs log {l_vol}) DOUBLE-WRITE suspect")
    elif l_fills == 0 and j_rows > 0:
        status = "MISMATCH"
        notes.append(f"log has no fills but journal has {j_rows} rows (log parsing broken?)")

    note = "; ".join(notes) if notes else "rows>=fills, volume within 5%"
    print(f"RECONCILE td={latest_td} status={status} logs={len(log_paths)} "
          f"journal(rows={j_rows},vol={j_vol}) log(fills={l_fills},vol={l_vol}) :: {note}")
    return 0 if status == "OK" else 2


if __name__ == "__main__":
    sys.exit(main())
