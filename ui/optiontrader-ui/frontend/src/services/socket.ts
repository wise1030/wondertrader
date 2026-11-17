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
        this.notifyStatus('connecting');
        try {
            const ws = new WebSocket(this.url);
            this.ws = ws;
            ws.onopen = () => {
                if (this.ws !== ws) { ws.close(); return; }
                this.isConnecting = false;
                this.notifyStatus('connected');
            };
            ws.onmessage = (event) => {
                if (this.ws !== ws) return;
                try {
                    this.notifyFrame(JSON.parse(event.data));
                } catch (e) {
                    console.error('[SocketService] bad frame:', e, event.data);
                }
            };
            ws.onclose = () => {
                if (this.ws !== ws) return;
                this.isConnecting = false;
                this.ws = null;
                this.notifyStatus('disconnected');
                if (this.shouldReconnect) setTimeout(() => this.connect(), this.reconnectInterval);
            };
            ws.onerror = () => { /* onclose handles state */ };
        } catch (e) {
            this.isConnecting = false;
            this.ws = null;
            this.notifyStatus('disconnected');
            if (this.shouldReconnect) setTimeout(() => this.connect(), this.reconnectInterval);
        }
    }

    send(line: string) {
        if (this.ws && this.ws.readyState === WebSocket.OPEN) this.ws.send(line);
        else console.warn('[SocketService] send dropped, not connected');
    }

    onFrame(h: FrameHandler) { this.frameHandlers.add(h); return () => { this.frameHandlers.delete(h); }; }
    onStatus(h: StatusHandler) { this.statusHandlers.add(h); return () => { this.statusHandlers.delete(h); }; }

    private notifyFrame(f: any) { this.frameHandlers.forEach(h => h(f)); }
    private notifyStatus(s: 'disconnected' | 'connecting' | 'connected') { this.statusHandlers.forEach(h => h(s)); }
}

export const socketService = new SocketService();
