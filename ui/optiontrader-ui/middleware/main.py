"""FastAPI middleware for the WtOptionCore P1 UI.

Single upstream: the strategy's UiBridge (127.0.0.1:8710, JSON Lines).
Serves the React SPA from ../frontend/dist and bridges browser WebSockets
to the upstream TCP connection. Snapshot frames get snapshotAgeMs injected
at forward time (consumer-side age — the only place it is meaningful).
"""
import asyncio
import json
import logging
import os
import time
from contextlib import asynccontextmanager

from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from fastapi.middleware.cors import CORSMiddleware
from fastapi.staticfiles import StaticFiles

from wt_option_client import WtOptionClient

logging.basicConfig(level=logging.INFO,
                    format="%(asctime)s %(name)s %(levelname)s %(message)s")
logger = logging.getLogger("Middleware")

UI_HOST = os.environ.get("WTOPTION_UI_HOST", "127.0.0.1")
UI_PORT = int(os.environ.get("WTOPTION_UI_PORT", "8710"))
LISTEN_PORT = int(os.environ.get("WTOPTION_LISTEN_PORT", "8002"))

upstream: WtOptionClient | None = None
connected_websockets: set[WebSocket] = set()
_loop: asyncio.AbstractEventLoop | None = None


def _broadcast(frame: dict):
    """Called from the upstream reader (any thread/loop context)."""
    now_ms = int(time.time() * 1000)
    if frame.get("type") == "snapshot":
        ts = frame.get("ts")
        frame["snapshotAgeMs"] = (now_ms - ts) if isinstance(ts, (int, float)) else None

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
    upstream = WtOptionClient(UI_HOST, UI_PORT, _broadcast)
    task = asyncio.create_task(upstream.connect())
    logger.info(f"Upstream UiBridge: {UI_HOST}:{UI_PORT}, listening on :{LISTEN_PORT}")
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


@app.websocket("/ws")
async def websocket_endpoint(websocket: WebSocket):
    await websocket.accept()
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
    uvicorn.run(app, host="127.0.0.1", port=LISTEN_PORT)
