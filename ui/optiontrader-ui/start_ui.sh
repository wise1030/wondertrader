#!/usr/bin/env bash
# P1 UI middleware 启动（运维纪律与 dist/start.sh 一致：setsid nohup + kill -9 停止）
set -euo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PIDFILE="$DIR/middleware.pid"
LOG="$DIR/middleware.log"

stop() {
    if [ -f "$PIDFILE" ]; then
        kill -9 "$(cat "$PIDFILE")" 2>/dev/null || true
        rm -f "$PIDFILE"
        echo "stopped"
    else
        echo "no pidfile (already stopped?)"
    fi
}

case "${1:-start}" in
    stop) stop; exit 0 ;;
    start) ;;
    *) echo "usage: $0 [start|stop]"; exit 2 ;;
esac

stop >/dev/null 2>&1 || true
cd "$DIR/middleware"
PY=$(command -v python3)
setsid nohup "$PY" main.py > "$LOG" 2>&1 < /dev/null &
echo $! > "$PIDFILE"
echo "middleware starting (pid $(cat "$PIDFILE")), log: $LOG"
