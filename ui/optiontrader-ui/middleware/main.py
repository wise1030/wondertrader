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
from fastapi.responses import PlainTextResponse, RedirectResponse
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

upstream: WtOptionClient | None = None
connected_websockets: set[WebSocket] = set()
_last_hello: dict | None = None   # most recent hello frame — replayed to new
                                  # browsers (audit batch B, M1: the bridge only
                                  # emits hello on TCP accept/auth, so a browser
                                  # refresh would otherwise stay readonly=true)
_loop: asyncio.AbstractEventLoop | None = None


def _broadcast(frame: dict):
    """Called from the upstream reader (any thread/loop context)."""
    global _last_hello
    now_ms = int(time.time() * 1000)
    if frame.get("type") == "snapshot":
        ts = frame.get("ts")
        frame["snapshotAgeMs"] = (now_ms - ts) if isinstance(ts, (int, float)) else None
    elif frame.get("type") == "hello":
        _last_hello = frame

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
    upstream = WtOptionClient(BRIDGE_HOST, BRIDGE_PORT, _broadcast)
    task = asyncio.create_task(upstream.connect())
    logger.info(f"Upstream UiBridge: {BRIDGE_HOST}:{BRIDGE_PORT}, listening on {LISTEN_HOST}:{LISTEN_PORT}")
    yield
    await upstream.stop()
    task.cancel()


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
    connected_websockets.add(websocket)
    logger.info(f"WS client connected (total {len(connected_websockets)})")
    if upstream:
        # New browser: force a fresh snapshot + fills history upstream.
        await upstream.send('{"cmd":"refresh","cmdSeq":1}')
    try:
        while True:
            data = await websocket.receive_text()
            if upstream:
                await upstream.send(data)
    except WebSocketDisconnect:
        logger.info("WS client disconnected")
    except Exception as e:
        logger.error(f"WS error: {e}")
    finally:
        connected_websockets.discard(websocket)


frontend_path = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "frontend", "dist")
if os.path.exists(frontend_path):
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
