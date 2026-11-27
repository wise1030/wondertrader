#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""reconcile_oqm_counters.py — 单合约单日 报/撤/成 对账（2026-11-27）

口径说明
--------
交易所按【交易日、单合约】统计撤单/报单量（做市商豁免处罚，但统计义务保留）。
本脚本对比两个独立数据源：

1. 柜台流水  generated/traders/simnow/orders.csv   （GBK，回报驱动）
   - action 口径：OL/OS/CL/CS = 报单（开多/开空/平多/平空）；CNL/CNS = 撤单
   - 报单数 = action ∈ {OL,OS,CL,CS} 的行数
   - 撤单数 = action ∈ {CNL,CNS} 的行数（柜台确认的撤单）
   - 成交数 = traded > 0 的报单行数（近似；部分成交按 1 单计）
   - 仅统计 date 列 == 目标交易日的行

2. OQM 计数  outputs_option/oqm_counters_<tdate>.csv
   code,cancels,newOrders,fills,rejects,lateFills,position
   （append-only，同日多次 dump —— 每 code 取【最后一行】= 最新累计）

用途
----
- 每日收盘后对账：OQM 计数 vs 柜台流水，差异超阈值告警（SimNow 丢回报时
  OQM 确认口径会低估 —— 以柜台 CSV 为准）。
- 输出单合约当日 撤单量 TOP N 与距 max_cancels_allowed(500) 的余量。

用法
----
  python3 reconcile_oqm_counters.py [--date 20261127] [--dir .] [--top 10] [--warn-diff 20]
退出码：0 = 一致；1 = 存在超阈值差异或文件缺失。
"""
import argparse
import csv
import os
import sys
from collections import defaultdict


def load_counter_csv(path):
    """OQM counters: append-only，last row wins。"""
    last = {}
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split(",")
            if len(parts) < 5:
                continue
            code = parts[0]
            try:
                vals = [int(x) for x in parts[1:6]]
            except ValueError:
                continue
            last[code] = vals  # cancels,newOrders,fills,rejects,lateFills
    return last


def load_orders_csv(path, tdate):
    """柜台订单流水（GBK）：per-contract 报/撤/成，仅目标交易日。"""
    stats = defaultdict(lambda: {"orders": 0, "cancels": 0, "fills": 0})
    with open(path, "rb") as fb:
        text = fb.read().decode("gbk", errors="replace").splitlines()
    reader = csv.DictReader(text)
    for row in reader:
        if (row.get("date") or "").strip() != tdate:
            continue
        code = (row.get("code") or "").strip()
        action = (row.get("action") or "").strip().upper()
        if not code:
            continue
        if action in ("OL", "OS", "CL", "CS"):
            stats[code]["orders"] += 1
            try:
                if float(row.get("traded") or 0) > 0:
                    stats[code]["fills"] += 1
            except ValueError:
                pass
        elif action in ("CNL", "CNS"):
            stats[code]["cancels"] += 1
    return stats


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--date", required=True, help="交易日 YYYYMMDD")
    ap.add_argument("--dir", default=".", help="WtRunnerOption 工作目录")
    ap.add_argument("--top", type=int, default=10, help="撤单量 TOP N")
    ap.add_argument("--warn-diff", type=int, default=20, help="差异告警阈值（笔）")
    ap.add_argument("--limit", type=int, default=500, help="交易所单合约单日撤单上限")
    args = ap.parse_args()

    oqm_path = os.path.join(args.dir, "outputs_option", f"oqm_counters_{args.date}.csv")
    orders_path = os.path.join(args.dir, "generated", "traders", "simnow", "orders.csv")

    rc = 0
    oqm = {}
    if os.path.exists(oqm_path):
        oqm = load_counter_csv(oqm_path)
    else:
        print(f"[WARN] OQM counter 文件缺失: {oqm_path}（进程未运行或 dump 未生效）")
        rc = 1

    if not os.path.exists(orders_path):
        print(f"[ERROR] 柜台流水缺失: {orders_path}")
        return 1
    broker = load_orders_csv(orders_path, args.date)

    codes = sorted(set(oqm) | set(broker))
    print(f"=== 单合约当日对账 {args.date} ===")
    print(f"{'code':<24} {'OQM撤':>7} {'柜台撤':>7} {'OQM报':>7} {'柜台报':>7} {'余量':>6}  备注")
    bad = 0
    for code in codes:
        o_cxl, o_ord = (oqm.get(code) or [0, 0])[:2]
        b = broker.get(code, {"orders": 0, "cancels": 0, "fills": 0})
        diff_c = abs(o_cxl - b["cancels"])
        diff_o = abs(o_ord - b["orders"])
        remain = args.limit - max(o_cxl, b["cancels"])
        notes = []
        if diff_c > args.warn_diff or diff_o > args.warn_diff:
            notes.append("DIFF!")
            bad += 1
            rc = 1
        if remain <= 100:
            notes.append(f"接近上限(余{remain})")
        if not oqm.get(code):
            notes.append("仅柜台有")
        if not broker.get(code):
            notes.append("仅OQM有")
        print(f"{code:<24} {o_cxl:>7} {b['cancels']:>7} {o_ord:>7} {b['orders']:>7} {remain:>6}  {';'.join(notes)}")

    print(f"\n=== 撤单量 TOP {args.top}（柜台口径）===")
    top = sorted(broker.items(), key=lambda kv: -kv[1]["cancels"])[: args.top]
    for code, s in top:
        print(f"  {code:<24} 撤 {s['cancels']:>5}  报 {s['orders']:>5}  成 {s['fills']:>4}")

    print(f"\n汇总: 合约数={len(codes)} 差异超阈值={bad} rc={rc}")
    return rc


if __name__ == "__main__":
    sys.exit(main())
