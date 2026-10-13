#!/bin/bash
# channel_watchdog.sh — 通道/管道 darkness 告警 (19.26 F1 外部层)
#
# 背景: 2026-09-24 实证两类静默故障, 进程存活但业务全黑:
#   A) TD settlement-confirm 卡死 → channel-ready 87min 未触发 → 主链路整体早退
#   B) 20:40 重启后 ParserCTP 未连上 SimNow 行情 → 整夜零行情
# 策略层只能观测 A 的 tick 侧; 本脚本从外部覆盖 A+B + 通用日志沉默。
#
# 检测 (交易日时段内, 周一至周五):
#   1. settlement-stuck: 日志出现 "Querying confirming state" 晚于最近一条
#      "Trading channel ready" → ALERT (A 类签名)
#   2. pipeline-dark: Runner 日志最新行距今 > 300s → ALERT (A 类通用)
#   3. md-down: Parser 日志最新行距今 > 300s → ALERT (B 类)
# 告警只写 Logs/channel_watchdog.log (不自动重启; 重启走 wt_restart.sh 人工/定时纪律)。
# cron: */2 * * * 1-5
set -u

DIST="${1:-/home/ubuntu/projects/wondertrader/dist}"
LOGDIR="$DIST/Logs"
ALERT_LOG="$LOGDIR/channel_watchdog.log"
RUNNER=$(ls -t "$LOGDIR"/Runner_*.log 2>/dev/null | head -1)
PARSER=$(ls -t "$LOGDIR"/Parser/parser_*.log 2>/dev/null | head -1)
NOW=$(date +%s)
DOW=$(date +%u)
HM=$(date +%H%M)
HM=$((10#$HM))   # 去前导零, 避免 [ "0900" -ge ... ] 八进制歧义/非法八进制报错

# 交易日时段 (ao/SHFE): 09:00-11:30 / 13:30-15:00 / 21:00-01:00
# 注意: bash case 不支持数值区间, 原 0900-1130 写法只匹配字面量 → 永不触发。
# HM 带前导零, 数值比较必须 10# 去八进制。
in_session=0
if { [ "$HM" -ge 900 ] && [ "$HM" -le 1130 ]; } ||
   { [ "$HM" -ge 1330 ] && [ "$HM" -le 1500 ]; } ||
   [ "$HM" -ge 2100 ] || [ "$HM" -le 59 ]; then
    in_session=1
fi
[ "$DOW" -ge 6 ] && [ "$HM" -lt 2100 ] && in_session=0   # 周末日盘休
[ "$DOW" -eq 7 ] && in_session=0

alert() {
    local kind="$1" detail="$2"
    # 同类告警 10min 去重
    if grep -q "$(date +%F).*WD-$kind" "$ALERT_LOG" 2>/dev/null; then
        local last
        last=$(grep "WD-$kind" "$ALERT_LOG" | tail -1 | grep -oE '^\[?[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}' | head -1)
        if [ -n "$last" ]; then
            local last_epoch
            last_epoch=$(date -d "$last" +%s 2>/dev/null || echo 0)
            [ $((NOW - last_epoch)) -lt 600 ] && return
        fi
    fi
    echo "[$(date '+%F %T')] WD-$kind $detail" >> "$ALERT_LOG"
}

[ "$in_session" -eq 0 ] && exit 0
[ -z "$RUNNER" ] && { alert NO-RUNNER-LOG "no Runner_*.log under $LOGDIR"; exit 0; }

# 1) settlement-stuck (A 类精确签名)
last_query=$(grep -a "Querying confirming state" "$RUNNER" | tail -1 | grep -oE '^\[[0-9]{4}\.[0-9]{2}\.[0-9]{2} [0-9:]{8}' | tr -d '[' | head -1)
last_ready=$(grep -a "Trading channel ready" "$RUNNER" | tail -1 | grep -oE '^\[[0-9]{4}\.[0-9]{2}\.[0-9]{2} [0-9:]{8}' | tr -d '[' | head -1)
if [ -n "$last_query" ]; then
    lq=$(date -d "${last_query//./-}" +%s 2>/dev/null || echo 0)
    lr=0
    [ -n "$last_ready" ] && lr=$(date -d "${last_ready//./-}" +%s 2>/dev/null || echo 0)
    if [ "$lq" -gt "$lr" ] && [ $((NOW - lq)) -gt 300 ]; then
        alert SETTLEMENT-STUCK "settlement-confirm at $last_query, no channel-ready since ($RUNNER)"
    fi
fi

# 2) pipeline-dark: Runner 日志沉默 > 300s
mtime=$(stat -c %Y "$RUNNER")
age=$((NOW - mtime))
[ "$age" -gt 300 ] && alert PIPELINE-DARK "Runner log silent ${age}s ($RUNNER)"

# 3) md-down: Parser 日志沉默 > 300s
if [ -n "$PARSER" ]; then
    pmtime=$(stat -c %Y "$PARSER")
    page=$((NOW - pmtime))
    [ "$page" -gt 300 ] && alert MD-DOWN "Parser log silent ${page}s ($PARSER)"
else
    alert MD-DOWN "no parser log found"
fi

exit 0
