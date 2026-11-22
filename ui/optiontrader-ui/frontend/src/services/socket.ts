// Socket singleton — reconnecting WebSocket to the middleware /ws endpoint.
// Handlers receive the full parsed frame (our protocol is flat, not nested).

type FrameHandler = (frame: any) => void;
type StatusHandler = (status: 'disconnected' | 'connecting' | 'connected') => void;

export class SocketService {
    private ws: WebSocket | null = null;
    private frameHandlers: Set<FrameHandler> = new Set();
    private statusHandlers: Set<StatusHandler> = new Set();
    private url: string;
    private reconnectInterval = 3000;
    private shouldReconnect = true;
    private isConnecting = false;
    // P1 fix (2026-11-18): the module-level singleton connects in its
    // constructor, BEFORE React's useEffect subscribes onStatus — the
    // 'connecting'/'connected' events were dropped and the UI stayed on the
    // initial 'disconnected' forever (while data flowed fine). Keep the
    // latest status and replay it to every new subscriber.
    private currentStatus: 'disconnected' | 'connecting' | 'connected' = 'disconnected';

    constructor() {
        const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
        let host = window.location.hostname || '127.0.0.1';
        let port = window.location.port;
        if (port === '5173' || !port) port = '8002';
        this.url = `${protocol}//${host}:${port}/ws`;
        this.connect();
    }

    connect() {
        if (this.isConnecting || (this.ws && (this.ws.readyState === WebSocket.OPEN || this.ws.readyState === WebSocket.CONNECTING))) return;
        this.shouldReconnect = true;
        this.isConnecting = true;
        this.setStatus('connecting');
        try {
            const ws = new WebSocket(this.url);
            this.ws = ws;
            // P1 (2026-11-19): if the handshake stalls (silent packet drop),
            // isConnecting blocks every retry and the UI sits on 'connecting'
            // for the browser's long internal timeout. Force a reset at 10s.
            const connectTimer = setTimeout(() => {
                if (this.ws === ws && ws.readyState === WebSocket.CONNECTING) {
                    try { ws.close(); } catch { /* ignore */ }
                    this.isConnecting = false;
                    this.ws = null;
                    this.setStatus('disconnected');
                    if (this.shouldReconnect) setTimeout(() => this.connect(), this.reconnectInterval);
                }
            }, 10000);
            ws.onopen = () => {
                clearTimeout(connectTimer);
                if (this.ws !== ws) { ws.close(); return; }
                this.isConnecting = false;
                this.setStatus('connected');
            };
            ws.onmessage = (event) => {
                if (this.ws !== ws) return;
                // P1 keepalive: answer middleware application pings so the
                // uplink packets keep the NAT/firewall session alive. Match
                // by parsed type — json.dumps output has spaces
                // ('{"type": "ping", ...}') that a raw substring misses.
                try {
                    const frame = JSON.parse(event.data);
                    if (frame && frame.type === 'ping') {
                        this.send(JSON.stringify({ type: 'pong' }));
                        return;
                    }
                    this.notifyFrame(frame);
                } catch (e) {
                    console.error('[SocketService] bad frame:', e, event.data);
                }
            };
            ws.onclose = () => {
                clearTimeout(connectTimer);
                if (this.ws !== ws) return;
                this.isConnecting = false;
                this.ws = null;
                this.setStatus('disconnected');
                if (this.shouldReconnect) setTimeout(() => this.connect(), this.reconnectInterval);
            };
            ws.onerror = () => { /* onclose handles state */ };
        } catch (e) {
            this.isConnecting = false;
            this.ws = null;
            this.setStatus('disconnected');
            if (this.shouldReconnect) setTimeout(() => this.connect(), this.reconnectInterval);
        }
    }

    /// Returns false when the line was dropped (socket down) — callers that
    /// claim "order sent" MUST check this (audit 11-22: the UI used to toast
    /// "sent" even when the WebSocket was disconnected).
    send(line: string): boolean {
        if (this.ws && this.ws.readyState === WebSocket.OPEN) { this.ws.send(line); return true; }
        console.warn('[SocketService] send dropped, not connected');
        return false;
    }

    onFrame(h: FrameHandler) { this.frameHandlers.add(h); return () => { this.frameHandlers.delete(h); }; }
    // Subscribe AND replay the latest status — closes the constructor-race.
    onStatus(h: StatusHandler) {
        this.statusHandlers.add(h);
        h(this.currentStatus);
        return () => { this.statusHandlers.delete(h); };
    }

    private setStatus(s: 'disconnected' | 'connecting' | 'connected') {
        if (s === this.currentStatus) return;   // diag: report transitions only
        this.currentStatus = s;
        this.notifyStatus(s);
        // Diagnostic uplink: report connection-state transitions to the
        // middleware log so support can see the browser-side state machine
        // without asking the user for screenshots.
        try {
            if (this.ws && this.ws.readyState === WebSocket.OPEN)
                this.ws.send(JSON.stringify({ type: 'conn_state', status: s, url: this.url }));
        } catch { /* never let diagnostics break the socket */ }
    }
    private notifyFrame(f: any) { this.frameHandlers.forEach(h => h(f)); }
    private notifyStatus(s: 'disconnected' | 'connecting' | 'connected') { this.statusHandlers.forEach(h => h(s)); }
}

export const socketService = new SocketService();
