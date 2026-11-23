"""FastAPI middleware for the WtOptionCore P1 UI.

Single upstream: the strategy's UiBridge (127.0.0.1:8710, JSON Lines).
Serves the React SPA from ../frontend/dist and bridges browser WebSockets
to the upstream TCP connection. Snapshot frames get snapshotAgeMs injected
at forward time (consumer-side age — the only place it is meaningful).

Optional internet exposure: set WTOPTION_UI_TOKEN to require a shared token
(?token=... on first visit sets a cookie; /ws requires the cookie).
Bridge stays loopback-only — only this middleware faces the network.
"""
import asyncio
import json
import logging
import os
import secrets
import time
from contextlib import asynccontextmanager

from fastapi import FastAPI, Request, WebSocket, WebSocketDisconnect
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import JSONResponse, PlainTextResponse, RedirectResponse
from fastapi.staticfiles import StaticFiles

from wt_option_client import WtOptionClient

logging.basicConfig(level=logging.INFO,
                    format="%(asctime)s %(name)s %(levelname)s %(message)s")
logger = logging.getLogger("Middleware")

BRIDGE_HOST = os.environ.get("WTOPTION_BRIDGE_HOST", "127.0.0.1")  # UiBridge stays loopback
BRIDGE_PORT = int(os.environ.get("WTOPTION_UI_PORT", "8710"))
LISTEN_HOST = os.environ.get("WTOPTION_LISTEN_HOST", "127.0.0.1")   # 0.0.0.0 = expose
LISTEN_PORT = int(os.environ.get("WTOPTION_LISTEN_PORT", "8002"))
UI_TOKEN = os.environ.get("WTOPTION_UI_TOKEN", "")              # empty = no auth (loopback only!)

# P1 batch-5.5: optional alert webhook.  Strategy-side alerts (order
# anomalies, risk events — already 2s rate-limited) are POSTed as JSON to
# this URL when set; empty = disabled.  Only warn/error levels (>=1) fire.
ALERT_WEBHOOK_URL = os.environ.get("WTOPTION_ALERT_WEBHOOK", "")
ALERT_WEBHOOK_MIN_LEVEL = int(os.environ.get("WTOPTION_ALERT_WEBHOOK_MIN_LEVEL", "1"))

upstream: WtOptionClient | None = None
connected_websockets: set[WebSocket] = set()
# P2 (audit 11-23): REST POST /api/param ack correlation — cmdSeq -> Future
_pending_acks: dict[int, asyncio.Future] = {}
# Audit 11-22: the UI footer's "bridge" lamp used to be fed by a conn_state
# frame that was NEVER produced (only browser->middleware diagnostics were
# logged), so it showed a permanent red "unknown".  Track and broadcast the
# real middleware<->UiBridge link state.
_upstream_status = "disconnected"
_last_hello: dict | None = None   # most recent hello frame — replayed to new
                                  # browsers (audit batch B, M1: the bridge only
                                  # emits hello on TCP accept/auth, so a browser
                                  # refresh would otherwise stay readonly=true)
_loop: asyncio.AbstractEventLoop | None = None


async def _post_alert_webhook(frame: dict):
    """Fire-and-forget POST of a graded alert to the operator webhook."""
    import httpx
    try:
        async with httpx.AsyncClient(timeout=5.0) as client:
            await client.post(ALERT_WEBHOOK_URL, json=frame)
    except Exception as exc:
        logger.warning("alert webhook POST failed: %s", exc)


def _upstream_state(status: str):
    global _upstream_status
    if status == _upstream_status:
        return
    _upstream_status = status
    logger.info("UiBridge link: %s", status)
    _broadcast({"type": "conn_state", "status": status, "scope": "bridge"})


def _broadcast(frame: dict):
    """Called from the upstream reader (any thread/loop context)."""
    global _last_hello
    # P2: resolve REST /api/param waiters before the general fan-out.
    if frame.get("type") == "ack":
        fut = _pending_acks.pop(int(frame.get("cmdSeq", -1)), None)
        if fut is not None and not fut.done():
            try:
                fut.set_result(frame)
            except Exception:
                pass
    now_ms = int(time.time() * 1000)
    if frame.get("type") == "snapshot":
        ts = frame.get("ts")
        frame["snapshotAgeMs"] = (now_ms - ts) if isinstance(ts, (int, float)) else None
    elif frame.get("type") == "hello":
        _last_hello = frame
    elif frame.get("type") == "alert" and ALERT_WEBHOOK_URL:
        # batch-5.5: remote push — warn/error levels only, off by default
        try:
            if int(frame.get("level", 0)) >= ALERT_WEBHOOK_MIN_LEVEL and _loop and _loop.is_running():
                _loop.call_soon_threadsafe(
                    lambda: asyncio.ensure_future(_post_alert_webhook(frame)))
        except (TypeError, ValueError):
            pass

    async def _push():
        dead = []
        for ws in list(connected_websockets):
            try:
                await ws.send_json(frame)
            except Exception:
                dead.append(ws)
        for ws in dead:
            connected_websockets.discard(ws)

    if _loop and _loop.is_running():
        _loop.call_soon_threadsafe(lambda: asyncio.ensure_future(_push()))


@asynccontextmanager
async def lifespan(app: FastAPI):
    global upstream, _loop
    _loop = asyncio.get_running_loop()
    upstream = WtOptionClient(BRIDGE_HOST, BRIDGE_PORT, _broadcast, _upstream_state)
    task = asyncio.create_task(upstream.connect())
    # P1 (2026-11-18): application-layer keepalive. Public-path NAT/firewalls
    # expire the WS session on downlink-only traffic (observed irregular
    # drops every ~20s-2.5min on the browser path while the loopback upstream
    # stayed up). A periodic ping that the frontend answers with a pong keeps
    # uplink packets flowing and the middlebox session alive.
    async def _ping_loop():
        while True:
            await asyncio.sleep(25)
            if connected_websockets:
                await _push_to_all({"type": "ping", "ts": int(time.time() * 1000)})
    ping_task = asyncio.create_task(_ping_loop())
    logger.info(f"Upstream UiBridge: {BRIDGE_HOST}:{BRIDGE_PORT}, listening on {LISTEN_HOST}:{LISTEN_PORT}")
    yield
    ping_task.cancel()
    await upstream.stop()
    task.cancel()


async def _push_to_all(frame: dict):
    dead = []
    for ws in list(connected_websockets):
        try:
            await ws.send_json(frame)
        except Exception:
            dead.append(ws)
    for ws in dead:
        connected_websockets.discard(ws)


app = FastAPI(lifespan=lifespan)
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

TOKEN_COOKIE = "wtoption_ui"


async def _token_guard(request: Request, call_next):
    """Shared-secret gate for internet exposure. Without WTOPTION_UI_TOKEN
    (loopback deployments) this is a pass-through."""
    if not UI_TOKEN:
        return await call_next(request)
    q = request.query_params.get("token", "")
    if q and secrets.compare_digest(q, UI_TOKEN):
        # first visit: plant the cookie (WS handshake only carries cookies,
        # not query params — so the browser must visit ?token= first)
        if request.url.path == "/ws":
            return PlainTextResponse("unauthorized: open /?token=... first", 403)
        resp = RedirectResponse(url="/", status_code=302) \
            if request.url.path != "/" else await call_next(request)
        resp.set_cookie(TOKEN_COOKIE, UI_TOKEN, max_age=7 * 86400, httponly=True)
        return resp
    cookie = request.cookies.get(TOKEN_COOKIE, "")
    if cookie and secrets.compare_digest(cookie, UI_TOKEN):
        return await call_next(request)
    return PlainTextResponse(
        "unauthorized: append ?token=<WTOPTION_UI_TOKEN> once", status_code=403)


app.middleware("http")(_token_guard)


@app.websocket("/ws")
async def websocket_endpoint(websocket: WebSocket):
    if UI_TOKEN:
        cookie = websocket.cookies.get(TOKEN_COOKIE, "")
        if not (cookie and secrets.compare_digest(cookie, UI_TOKEN)):
            await websocket.close(code=4401)
            return
    await websocket.accept()
    if _last_hello:
        # M1: replay the cached hello so a refreshed browser learns sid/readonly
        # immediately (the bridge will not re-send it until upstream reconnects).
        try:
            await websocket.send_text(json.dumps(_last_hello))
        except Exception:
            return
    # Current upstream state at accept time (the transition broadcast may have
    # happened before this client joined).
    try:
        await websocket.send_text(json.dumps(
            {"type": "conn_state", "status": _upstream_status, "scope": "bridge"}))
    except Exception:
        return
    connected_websockets.add(websocket)
    logger.info(f"WS client connected (total {len(connected_websockets)})")
    if upstream:
        # New browser: force a fresh snapshot + fills history upstream.
        await upstream.send('{"cmd":"refresh","cmdSeq":1}')
    try:
        while True:
            data = await websocket.receive_text()
            # P1 keepalive: swallow the frontend's pong replies instead of
            # forwarding them upstream as commands.
            if data.strip() == '{"type":"pong"}':
                continue
            # Diagnostic: browser-side connection-state transitions.
            if '"conn_state"' in data:
                logger.info(f"BrowserConn {websocket.client}: {data.strip()[:120]}")
                continue
            if upstream:
                await upstream.send(data)
    except WebSocketDisconnect:
        logger.info("WS client disconnected")
    except Exception as e:
        logger.error(f"WS error: {e}")
    finally:
        connected_websockets.discard(websocket)


frontend_path = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "frontend", "dist")
# P2 (audit 11-23): REST hot-param entry — reaches the strategy's
# ParamRegistry through the same SETPARAM path the UI uses.  Requires the
# token cookie (or ?token= on this request via the guard).
@app.post("/api/param")
async def api_param(body: dict):
    if upstream is None or not upstream.connected:
        return JSONResponse({"ok": False, "err": "upstream not connected"}, status_code=503)
    key = str(body.get("key", "")).strip()
    if not key:
        return JSONResponse({"ok": False, "err": "key required"}, status_code=400)
    try:
        value = float(body["value"])
    except (KeyError, TypeError, ValueError):
        return JSONResponse({"ok": False, "err": "numeric value required"}, status_code=400)
    cmd_seq = int(time.time() * 1000) % 1000000000
    loop = asyncio.get_running_loop()
    fut = loop.create_future()
    _pending_acks[cmd_seq] = fut
    await upstream.send(json.dumps({"cmd": "set_param", "key": key, "value": value, "cmdSeq": cmd_seq}))
    try:
        ack = await asyncio.wait_for(fut, timeout=2.5)
    except asyncio.TimeoutError:
        _pending_acks.pop(cmd_seq, None)
        return JSONResponse({"ok": False, "err": "timeout waiting for strategy ack"}, status_code=504)
    return {"ok": bool(ack.get("ok")), "err": ack.get("err", ""), "key": key, "value": value}


if os.path.exists(frontend_path):
    from fastapi.responses import FileResponse

    # P1 (2026-11-18): StaticFiles sends NO Cache-Control, so browsers keep
    # the html on a HEURISTIC cache and a plain refresh can silently reuse an
    # OLD index.html (pointing at a stale hashed JS bundle — the conn-stuck
    # bug lived on for days because of this). no-cache makes the browser
    # revalidate the shell every time; the hashed /assets files stay
    # long-cache safe (content change => new hash => new URL).
    @app.get("/", include_in_schema=False)
    async def index_no_cache():
        resp = FileResponse(os.path.join(frontend_path, "index.html"))
        resp.headers["Cache-Control"] = "no-cache, must-revalidate"
        return resp

    # ------------------------------------------------------------------
    # P1 batch-6.2/6.3: analysis series API — the strategy's AnalysisRecorder
    # sidecars (generated/volseries_<tdate>.csv + pfseries_<tdate>.csv) as
    # JSON for the vol-cone and attribution panels.  Auth-gated by the same
    # token middleware as everything else.
    # ------------------------------------------------------------------
    SERIES_DIR = os.environ.get(
        "WTOPTION_SERIES_DIR",
        "/home/ubuntu/projects/wondertrader/dist/WtRunnerOption/generated")

    def _read_series_csv(name: str, tdate: str) -> tuple[list[list[float]], int]:
        import csv
        import glob
        if tdate == "recent":
            # multi-day history for the vol cone: newest N day files merged
            paths = sorted(glob.glob(os.path.join(SERIES_DIR, f"{name}_*.csv")),
                           reverse=True)[:15]
        else:
            paths = [os.path.join(SERIES_DIR, f"{name}_{tdate}.csv")]
        paths = [p for p in paths if os.path.exists(p)]
        rows: list[list[float]] = []
        for path in paths:
            try:
                with open(path, newline="") as fh:
                    for row in csv.DictReader(fh):
                        try:
                            rows.append([float(v) for v in row.values()])
                        except (TypeError, ValueError):
                            continue
            except OSError:
                continue
        rows.sort(key=lambda r: r[0])   # by ts — cone percentiles need no order,
        return rows, len(paths)         # but charts read better chronological

    @app.get("/api/series")
    async def series(tdate: str, kind: str = "vol"):
        """kind=vol -> volseries rows [ts,expiry,atmvol,fwd,dte];
        kind=pf  -> pfseries rows [ts,delta,gamma,vega,theta,pnl,spot,atmvol].
        Returns {"columns": [...], "rows": [...]} — empty rows before the
        first recorded minute."""
        cols = {
            "vol": ["ts", "expiry", "atmvol", "fwd", "dte"],
            "pf": ["ts", "delta", "gamma", "vega", "theta", "pnl", "spot", "atmvol"],
        }
        if kind not in cols:
            return PlainTextResponse("kind must be vol|pf", status_code=400)
        if tdate != "recent" and not (tdate.isdigit() and len(tdate) <= 8):
            return PlainTextResponse("tdate must be YYYYMMDD or recent", status_code=400)
        rows, days = _read_series_csv("volseries" if kind == "vol" else "pfseries", tdate)

        return {"columns": cols[kind], "rows": rows, "days": days}

    app.mount("/", StaticFiles(directory=frontend_path, html=True), name="static")
else:
    logger.warning(f"frontend/dist not found at {frontend_path} — API-only mode")

if __name__ == "__main__":
    import uvicorn
    if LISTEN_HOST != "127.0.0.1" and not UI_TOKEN:
        logger.error("Refusing to listen on non-loopback without WTOPTION_UI_TOKEN")
        raise SystemExit(2)
    logger.info(f"Serving on {LISTEN_HOST}:{LISTEN_PORT} (auth={'on' if UI_TOKEN else 'OFF'})")
    uvicorn.run(app, host=LISTEN_HOST, port=LISTEN_PORT)
