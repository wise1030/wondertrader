#!/usr/bin/env python3
"""Toxicity episode 离线分析脚本 (V10-R11 Step-1)

输入:
  --log     Runner.log 路径 (回测 outputs/Runner.log 或生产 Logs/Runner_YYYY-MM-DD.log)
  --trades  trades.csv 路径 (回测 outputs_bt/uft/trades.csv 或生产
            generated/outputs/futu_mm_ao_live/trades.csv, convert_trades.py 产物)
输出 (stdout):
  1. episode 统计: 次数/总暂停时长/时长分布/peak 分布
  2. markout 分析: "引线窗口"(episode enter 前 K 秒)内成交 vs 基准成交的
     +10s/+30s/+60s 符号化价格偏移 (tick 单位); 若引线成交的 markout 系统性为负
     (买后即跌/卖后即涨) => 检测器预测了逆向选择 => 有效
  3. 暂停期间成交数 (应≈0, 验证暂停生效)
  4. 分数分布: debug %50 采样行的 score/vpin 分位数 (p50/p90/p95/p99)
     — 服务演进路线 step-2 的阈值重标定

注意:
  - 持仓域格式 (实盘 convert_trades): LONG+OPEN/SHORT+CLOSE=buy; LONG+CLOSE/
    SHORT+OPEN=sell; 回测引擎 CSV: direct=BUY/SELL 即成交方向。脚本自动识别。
  - episode 是全局相位 (单一 TradingState), enter 行的 code 仅为触发合约,
    窗口对全部合约生效。
"""
import argparse
import re
import statistics
from bisect import bisect_left

LOG_TS = re.compile(r"\[(\d{4})\.(\d{2})\.(\d{2}) (\d{2}):(\d{2}):(\d{2})")
EP_ENTER = re.compile(r"\[TOXIC-EPISODE\] (\S+) enter")
EP_EXIT = re.compile(r"\[TOXIC-EPISODE\] exit (?:duration=(\d+)ms )?(?:session_reset )?peak=([\d.]+)")
TOXIC_SCORE = re.compile(r"\[TOXIC\] (\S+) score=([\d.]+) vpin=([\d.]+) is_toxic=(\w+)")


def log_ts_seconds(m):
    y, mo, d, h, mi, s = (int(x) for x in m.groups())
    return (y * 10000 + mo * 100 + d) * 86400 + h * 3600 + mi * 60 + s  # 伪 epoch (仅同日比较安全: 跨日用日期分量偏移)


def parse_log(path):
    episodes = []  # (enter_ts, exit_ts, peak, code)
    cur_enter = None
    cur_code = None
    cur_peak = 0.0
    scores = []
    vpins = []
    with open(path, errors="replace") as f:
        for line in f:
            m = LOG_TS.search(line)
            if not m:
                continue
            ts = log_ts_seconds(m)
            me = EP_ENTER.search(line)
            if me:
                if cur_enter is None:
                    cur_enter, cur_code, cur_peak = ts, me.group(1), 0.0
                continue
            mx = EP_EXIT.search(line)
            if mx and cur_enter is not None:
                if mx.group(1):  # duration ms
                    exit_ts = cur_enter + int(mx.group(1)) / 1000.0
                else:  # session_reset
                    exit_ts = ts
                episodes.append((cur_enter, exit_ts, float(mx.group(2)), cur_code))
                cur_enter = None
                continue
            ms = TOXIC_SCORE.search(line)
            if ms:
                scores.append(float(ms.group(2)))
                vpins.append(float(ms.group(3)))
    if cur_enter is not None:  # 未闭合 episode (进程被杀等)
        episodes.append((cur_enter, None, cur_peak, cur_code))
    return episodes, scores, vpins


def parse_trades(path, tick_size):
    fills = {}  # code -> [(ts, side(+1 buy/-1 sell), price)]
    with open(path, errors="replace") as f:
        header = f.readline()
        if "direct" not in header:
            raise SystemExit("trades.csv 格式不识别: 缺 direct 列")
        for line in f:
            c = line.strip().split(",")
            if len(c) < 6:
                continue
            code, tstr, direct, action = c[0], c[1], c[2].strip(), c[3].strip()
            try:
                price, qty = float(c[4]), float(c[5])
            except ValueError:
                continue
            if qty <= 0:
                continue
            # 时间: YYYYMMDDHHMMSS
            t = int(tstr)
            ts = (t // 1000000) * 86400 + (t % 1000000) // 10000 * 3600 + (t % 10000) // 100 * 60 + t % 100
            if direct in ("BUY", "SELL"):
                side = 1 if direct == "BUY" else -1
            else:  # 持仓域: LONG/SHORT + OPEN/CLOSE
                side = 1 if (direct == "LONG" and action == "OPEN") or (direct == "SHORT" and action == "CLOSE") else -1
            fills.setdefault(code, []).append((ts, side, price))
    for v in fills.values():
        v.sort()
    return fills


def markouts(fills_list, horizons, tick_size):
    """对每个成交算 horizons 后的符号化偏移 (ticks)。用之后最近的成交价。"""
    times = [t for t, _, _ in fills_list]
    out = {h: [] for h in horizons}
    for i, (ts, side, price) in enumerate(fills_list):
        for h in horizons:
            j = bisect_left(times, ts + h, lo=i + 1)
            if j >= len(times):
                continue
            _, _, p2 = fills_list[j]
            out[h].append(side * (p2 - price) / tick_size)  # 买: 涨为正; 卖: 跌为正
    return out


def fmt_stats(vals, tick_size):
    if not vals:
        return "n=0"
    vals = sorted(vals)
    mean = statistics.mean(vals)
    med = statistics.median(vals)
    neg = sum(1 for v in vals if v < 0) / len(vals) * 100
    return f"n={len(vals)} mean={mean:+.2f}t med={med:+.2f}t adverse%={neg:.0f}%"


def pct(vals, q):
    if not vals:
        return float("nan")
    vals = sorted(vals)
    k = (len(vals) - 1) * q / 100
    f = int(k)
    return vals[f] + (k - f) * (vals[min(f + 1, len(vals) - 1)] - vals[f])


def main():
    ap = argparse.ArgumentParser(description="Toxicity episode markout 离线分析")
    ap.add_argument("--log", required=True)
    ap.add_argument("--trades", required=True)
    ap.add_argument("--lead-seconds", type=int, default=30, help="引线窗口 K 秒 (episode enter 前)")
    ap.add_argument("--tick", type=float, default=1.0, help="tick size (ao=1.0)")
    ap.add_argument("--horizons", type=int, nargs="+", default=[10, 30, 60])
    args = ap.parse_args()

    episodes, scores, vpins = parse_log(args.log)
    fills = parse_trades(args.trades, args.tick)

    closed = [(e, x, p, c) for e, x, p, c in episodes if x is not None]
    durs = [x - e for e, x, _, _ in closed]
    peaks = [p for _, _, p, _ in closed]

    print("=== 1. Episode 统计 ===")
    print(f"episodes={len(episodes)} (闭合={len(closed)})")
    if durs:
        print(f"暂停总时长={sum(durs)/60:.1f}min  mean={statistics.mean(durs):.0f}s "
              f"p50={pct(durs,50):.0f}s p90={pct(durs,90):.0f}s p99={pct(durs,99):.0f}s")
    if peaks:
        print(f"peak: p50={pct(peaks,50):.2f} p90={pct(peaks,90):.2f} p99={pct(peaks,99):.2f}")

    # episode 窗口 (全局相位, 对所有合约生效)
    win = [(e, x) for e, x, _, _ in closed]

    def in_window(ts, lo_bias=0):
        for e, x in win:
            if e - lo_bias <= ts < x:
                return True
        return False

    print("\n=== 2. Markout 分析 (引线窗口成交 vs 基准) ===")
    lead_fills, base_fills, in_ep = [], [], 0
    for code, fl in fills.items():
        for t, side, price in fl:
            if in_window(t):
                in_ep += 1
            elif any(e - args.lead_seconds <= t < e for e, _, _, _ in closed):
                lead_fills.append((code, t, side, price))
            else:
                base_fills.append((code, t, side, price))

    for h in args.horizons:
        lead_m = markouts([(t, s, p) for _, t, s, p in lead_fills], [h], args.tick)[h]
        base_m = markouts([(t, s, p) for _, t, s, p in base_fills], [h], args.tick)[h]
        print(f"  +{h}s 引线成交: {fmt_stats(lead_m, args.tick)}")
        print(f"  +{h}s 基准成交: {fmt_stats(base_m, args.tick)}")
    print(f"  暂停期间成交数={in_ep} (应≈0=暂停生效; >0 说明相位阻断有漏)")

    print("\n=== 3. 分数分布 (debug %50 采样) ===")
    print(f"score: n={len(scores)} p50={pct(scores,50):.3f} p90={pct(scores,90):.3f} "
          f"p95={pct(scores,95):.3f} p99={pct(scores,99):.3f}")
    print(f"vpin : n={len(vpins)} p50={pct(vpins,50):.3f} p90={pct(vpins,90):.3f} "
          f"p95={pct(vpins,95):.3f} p99={pct(vpins,99):.3f}")
    print("(步2阈值重标定依据: enter 应取 p99 附近, exit 取 p95 附近)")


if __name__ == "__main__":
    main()
