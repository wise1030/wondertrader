import React, { createContext, useContext, useEffect, useMemo, useReducer, useRef, useCallback } from 'react';
import { socketService } from '../services/socket';
import type { Snapshot, OrderView, FillView, AckEvent } from '../types';

export interface MsgLog {
    time: string;
    type: string;
    content: string;
}

interface DataState {
    connectionStatus: 'disconnected' | 'connecting' | 'connected';
    snapshot: Snapshot | null;
    lastSnapshotSeq: number;
    snapshotArrivalMs: number;   // local Date.now() at frame arrival — live-age base (H1)
    orders: Record<number, OrderView>;
    fills: FillView[];
    msgLogs: MsgLog[];
    lastSnapLogMs: number;      // snapshot-log throttle (design §7: no 1Hz flood)
    sid: string;
    readonly: boolean;
}

const MAX_LOGS = 200;
const MAX_FILLS = 500;

const now = () => new Date().toLocaleTimeString('en-GB', { hour12: false });

function log(state: DataState, type: string, content: string): Partial<DataState> {
    return { msgLogs: [...state.msgLogs.slice(-(MAX_LOGS - 1)), { time: now(), type, content }] };
}

const initialState: DataState = {
    connectionStatus: 'disconnected',
    snapshot: null,
    lastSnapshotSeq: 0,
    snapshotArrivalMs: 0,
    orders: {},
    fills: [],
    msgLogs: [],
    lastSnapLogMs: 0,
    sid: '',
    readonly: true,
};

type Action =
    | { type: 'CONNECTION'; status: DataState['connectionStatus'] }
    | { type: 'HELLO'; sid: string; readonly: boolean }
    | { type: 'SNAPSHOT'; snap: Snapshot; arrivalMs: number }
    | { type: 'ORDER'; ev: any }
    | { type: 'TRADE'; ev: any }
    | { type: 'REJECT'; ev: any }
    | { type: 'ACK'; ev: AckEvent }
    | { type: 'FILLS'; items: FillView[] };

function reducer(state: DataState, a: Action): DataState {
    switch (a.type) {
        case 'CONNECTION':
            return { ...state, connectionStatus: a.status };
        case 'HELLO':
            return { ...state, sid: a.sid, readonly: a.readonly, ...log(state, 'hello', `sid=${a.sid} readonly=${a.readonly}`) };
        case 'SNAPSHOT': {
            const orders: Record<number, OrderView> = {};
            for (const o of a.snap.orders) orders[o.id] = o;
            // Live staleness (H1): snapshotAgeMs is injected by the middleware
            // at forward time (clock-skew-free base); components add the local
            // time elapsed since arrival via snapshotArrivalMs.
            // Log policy (design §7 — "不引入洪水"): first frame, stale (>3s)
            // or a 30s heartbeat line only; not one line per snapshot.
            const stale = (a.snap.snapshotAgeMs ?? 0) > 3000;
            const doLog = !state.snapshot || stale
                || a.arrivalMs - state.lastSnapLogMs >= 30000;
            return {
                ...state,
                snapshot: a.snap,
                lastSnapshotSeq: a.snap.seq,
                snapshotArrivalMs: a.arrivalMs,
                orders,
                lastSnapLogMs: doLog ? a.arrivalMs : state.lastSnapLogMs,
                ...(doLog ? log(state, 'snapshot',
                    `seq=${a.snap.seq} age=${a.snap.snapshotAgeMs ?? '?'}ms orders=${a.snap.orders.length}${stale ? ' STALE' : ''}`) : {}),
            };
        }
        case 'ORDER': {
            const ev = a.ev;
            // `<` (not `<=`): an event may share the seq of the last published
            // snapshot yet still be fresher than it (pushed between that
            // batch's completion and the next) — only strictly older seqs are
            // stale replay/backlog.
            if (ev.seq < state.lastSnapshotSeq) return state;
            const orders = { ...state.orders };
            const done = ev.canceled || ev.left <= 0;
            if (done) delete orders[ev.id];
            else orders[ev.id] = { id: ev.id, code: ev.code, buy: ev.buy, px: ev.px, qty: ev.qty, filled: Math.max(0, ev.qty - ev.left), t: ev.ts / 1000 };
            return { ...state, orders };
        }
        case 'TRADE': {
            const ev = a.ev;
            if (ev.seq < state.lastSnapshotSeq) return state;
            const fills = [...state.fills, { code: ev.code, buy: ev.buy, px: ev.px, qty: ev.qty, t: ev.ts / 1000 }];
            return { ...state, fills: fills.slice(-MAX_FILLS) };
        }
        case 'REJECT': {
            // Same watermark discipline as ORDER/TRADE — a stale reject must
            // not delete a live order from the authoritative view.
            if (a.ev.seq < state.lastSnapshotSeq) return state;
            const orders = { ...state.orders };
            delete orders[a.ev.id];
            return { ...state, orders, ...log(state, 'reject', `id=${a.ev.id} (entrust failed)`) };
        }
        case 'ACK':
            return { ...state, ...log(state, a.ev.ok ? 'ack' : 'error', `ack cmdSeq=${a.ev.cmdSeq} ok=${a.ev.ok}${a.ev.err ? ' err=' + a.ev.err : ''}`) };
        case 'FILLS':
            return { ...state, fills: a.items.slice(-MAX_FILLS), ...log(state, 'fills', `history ${a.items.length} fills`) };
        default:
            return state;
    }
}

interface DataContextValue extends DataState {
    sendCommand: (line: string) => void;
    refresh: () => void;
}

const DataContext = createContext<DataContextValue | null>(null);

export const DataProvider: React.FC<{ children: React.ReactNode }> = ({ children }) => {
    const [state, dispatch] = useReducer(reducer, initialState);
    // Batch bursts (event storms) into one dispatch per 100ms flush.
    const pending = useRef<Action[]>([]);

    const flush = useCallback(() => {
        const batch = pending.current;
        if (batch.length === 0) return;
        pending.current = [];
        // Fold the batch manually to avoid one re-render per event.
        dispatch(batch.length === 1 ? batch[0] : ((s: DataState) => batch.reduce(reducer, s)) as any);
    }, []);

    useEffect(() => {
        const timer = setInterval(flush, 100);
        const offFrame = socketService.onFrame((frame) => {
            switch (frame?.type) {
                case 'hello': pending.current.push({ type: 'HELLO', sid: frame.sid, readonly: frame.readonly }); break;
                case 'snapshot': pending.current.push({ type: 'SNAPSHOT', snap: frame, arrivalMs: Date.now() }); break;
                case 'order': pending.current.push({ type: 'ORDER', ev: frame }); break;
                case 'trade': pending.current.push({ type: 'TRADE', ev: frame }); break;
                case 'reject': pending.current.push({ type: 'REJECT', ev: frame }); break;
                case 'ack': pending.current.push({ type: 'ACK', ev: frame }); break;
                case 'fills': pending.current.push({ type: 'FILLS', items: frame.items ?? [] }); break;
                default: break;
            }
        });
        const offStatus = socketService.onStatus((status) => {
            pending.current.push({ type: 'CONNECTION', status });
        });
        return () => { clearInterval(timer); offFrame(); offStatus(); };
    }, [flush]);

    const sendCommand = useCallback((line: string) => socketService.send(line), []);
    const refresh = useCallback(() => socketService.send(JSON.stringify({ cmd: 'refresh', cmdSeq: Date.now() % 100000 })), []);

    const value = useMemo<DataContextValue>(() => ({ ...state, sendCommand, refresh }), [state, sendCommand, refresh]);
    return <DataContext.Provider value={value}>{children}</DataContext.Provider>;
};

export function useData(): DataContextValue {
    const ctx = useContext(DataContext);
    if (!ctx) throw new Error('useData outside DataProvider');
    return ctx;
}
