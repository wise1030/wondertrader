"""Asyncio TCP line client for the WtOptionCore UiBridge.

Speaks JSON Lines over loopback TCP (the strategy's P1 UI bridge): one JSON
object per line, both directions. Reconnects with exponential backoff
(1s -> 30s). The bridge is the single source of truth; this client just
forwards frames to the FastAPI layer and commands back upstream.
"""
import asyncio
import json
import logging

logger = logging.getLogger("WtOptionClient")


class WtOptionClient:
    def __init__(self, host: str, port: int, on_frame, on_status=None):
        """on_frame(frame: dict) is invoked for every parsed JSON line;
        on_status(status: str) reports bridge-link transitions
        (connecting / connected / disconnected) for the UI footer."""
        self.host = host
        self.port = port
        self.on_frame = on_frame
        self.on_status = on_status
        self._reader: asyncio.StreamReader | None = None
        self._writer: asyncio.StreamWriter | None = None
        self._stopping = False
        self.connected = False
        self._status = "disconnected"

    def _set_status(self, status: str):
        if status == self._status:
            return
        self._status = status
        if self.on_status:
            try:
                self.on_status(status)
            except Exception:
                logger.exception("on_status callback failed")

    async def connect(self):
        backoff = 1.0
        while not self._stopping:
            try:
                self._set_status("connecting")
                self._reader, self._writer = await asyncio.open_connection(
                    self.host, self.port)
                self.connected = True
                backoff = 1.0
                self._set_status("connected")
                logger.info(f"Connected to UiBridge at {self.host}:{self.port}")
                try:
                    async for raw in self._reader:
                        line = raw.decode("utf-8", errors="replace").strip()
                        if not line:
                            continue
                        try:
                            frame = json.loads(line)
                        except json.JSONDecodeError:
                            logger.warning(f"Dropping unparseable line: {line[:120]}")
                            continue
                        try:
                            self.on_frame(frame)
                        except Exception:
                            logger.exception("on_frame callback failed")
                finally:
                    self.connected = False
                    try:
                        if self._writer:
                            self._writer.close()
                    except Exception:
                        pass
                    self._set_status("disconnected")
                    logger.warning("UiBridge connection lost")
            except asyncio.CancelledError:
                raise
            except Exception as e:
                self._set_status("disconnected")
                logger.warning(f"UiBridge connect failed: {e}")
            if not self._stopping:
                await asyncio.sleep(backoff)
                backoff = min(backoff * 2.0, 30.0)

    async def send(self, line: str):
        """Send one command line upstream (no trailing newline needed)."""
        if not self._writer or not self.connected:
            logger.warning("send() dropped: upstream not connected")
            return
        try:
            self._writer.write(line.encode("utf-8") + b"\n")
            await self._writer.drain()
        except Exception as e:
            logger.warning(f"send() failed: {e}")

    async def stop(self):
        self._stopping = True
        self._set_status("disconnected")
        try:
            if self._writer:
                self._writer.close()
        except Exception:
            pass
