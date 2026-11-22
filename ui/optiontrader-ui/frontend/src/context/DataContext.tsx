import React, { createContext, useContext, useEffect, useMemo, useReducer, useRef, useCallback } from 'react';
import { socketService } from '../services/socket';
import { sound } from '../services/sound';
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
    flash: Record<string, number>;   // P1 batch-2: code -> Date.now() of latest fill (cell flash)
    panicked: boolean;
    msgLogs: MsgLog[];
    lastSnapLogMs: number;      // snapshot-log throttle (design §7: no 1Hz flood)
    sid: string;
    readonly: boolean;
    // P1: middleware bridge-link diagnostic (conn_state frame from the
    // middleware — shows the middleware↔UiBridge upstream state).
    bridgeConn: string;
    // P1 batch-5.5: latest graded alert — App toasts on change.
    lastAlert: { level: number; msg: string; ms: number } | null;
    // Audit 11-22: latest FAILED command ack.  Before this the only trace of
    // a rejected order/command was a line in the tiny message log while the
    // entry form had already toasted "sent" — the operator could believe a
    // dead order was working.
    lastAckError: { msg: string; ms: number } | null;
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
    flash: {},
    panicked: false,
    msgLogs: [],
    lastSnapLogMs: 0,
    sid: '',
    readonly: true,
    bridgeConn: 'unknown',
    lastAlert: null,
    lastAckError: null,
};

type Action =
    | { type: 'CONNECTION'; status: DataState['connectionStatus'] }
    | { type: 'HELLO'; sid: string; readonly: boolean }
    | { type: 'SNAPSHOT'; snap: Snapshot; arrivalMs: number }
    | { type: 'ORDER'; ev: any }
    | { type: 'TRADE'; ev: any }
    | { type: 'REJECT'; ev: any }
    | { type: 'ACK'; ev: AckEvent }
    | { type: 'FILLS'; items: FillView[] }
    | { type: 'ALERT'; level: number; msg: string; ms: number }
    | { type: 'CONN_STATE'; status: string };

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
            const panicked = !!a.snap?.state?.panicked;
            if (panicked && !state.panicked) sound.panic();
            // Audit 11-22: a page left open across the daily WtRunner restart
            // used to keep yesterday's fills/flash in memory; because fill
            // times are SIM seconds, yesterday's t=36000 and today's t=36000
            // were even deduped as the same fill.  A trading-date change now
            // starts a clean fill book (orders/snapshot are authoritative and
            // replaced anyway).
            const prevTdate = state.snapshot?.state?.tdate ?? 0;
            const newTdate = a.snap?.state?.tdate ?? 0;
            const dayRolled = prevTdate > 0 && newTdate > 0 && newTdate !== prevTdate;
            return {
                ...state,
                snapshot: a.snap,
                lastSnapshotSeq: a.snap.seq,
                snapshotArrivalMs: a.arrivalMs,
                panicked,
                orders,
                ...(dayRolled ? { fills: [], flash: {} } : {}),
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
            else orders[ev.id] = { id: ev.id, code: ev.code, buy: ev.buy, px: ev.px, qty: ev.qty, filled: Math.max(0, ev.qty - ev.left), t: ev.ts / 1000, tag: ev.tag };
            return { ...state, orders };
        }
        case 'TRADE': {
            const ev = a.ev;
            if (ev.seq < state.lastSnapshotSeq) return state;
            const fills = [...state.fills, { code: ev.code, buy: ev.buy, px: ev.px, qty: ev.qty, t: ev.ts / 1000, tag: ev.tag }];
            sound.trade();
            return { ...state, fills: fills.slice(-MAX_FILLS),
                     flash: { ...state.flash, [ev.code]: Date.now() } };
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
            return {
                ...state,
                lastAckError: a.ev.ok ? state.lastAckError
                    : { msg: a.ev.err || `command ${a.ev.cmdSeq} rejected`, ms: Date.now() },
                ...log(state, a.ev.ok ? 'ack' : 'error',
                    `ack cmdSeq=${a.ev.cmdSeq} ok=${a.ev.ok}${a.ev.err ? ' err=' + a.ev.err : ''}`),
            };
        case 'FILLS': {
            // P1 fix (2026-11-19): history used to REPLACE live fills — a
            // history frame arriving right after reconnect raced with fresh
            // trade events and erased them. Merge by dedup key (t+code+px+buy).
            // Audit 11-22: qty+tag in the key — two equal-priced partial fills
            // in the same second used to collapse into one on history merge.
            const key = (f: FillView) => `${f.t}|${f.code}|${f.px}|${f.qty}|${f.buy ? 1 : 0}|${f.tag ?? 0}`;
            const seen = new Set(state.fills.map(key));
            const merged = [...state.fills];
            for (const f of a.items) {
                if (!seen.has(key(f))) merged.push(f);
            }
            merged.sort((x, y) => (x.t ?? 0) - (y.t ?? 0));
            return { ...state, fills: merged.slice(-MAX_FILLS), ...log(state, 'fills', `history merged ${a.items.length}`) };
        }
        case 'ALERT': {
            // graded: level>=2 surfaces as an error line + toast trigger
            return { ...state, lastAlert: { level: a.level, msg: a.msg, ms: a.ms },
                     ...log(state, a.level >= 2 ? 'error' : 'info', `alert[${a.level}]: ${a.msg}`) };
        }
        case 'CONN_STATE':
            return { ...state, bridgeConn: a.status };
        default:
            return state;
    }
}

interface DataContextValue extends DataState {
    /// false = the line was dropped locally (socket down), NOT sent upstream.
    sendCommand: (line: string) => boolean;
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
        // P1 fix (2026-11-19): useReducer's dispatch does NOT execute a
        // function action like useState's updater — a function lands in the
        // reducer's switch and falls through `default`, silently dropping
        // the WHOLE batch. That swallowed the CONNECTION action whenever it
        // queued behind hello/snapshot in the same 100ms window (every page
        // load), leaving conn stuck at 'disconnected' while data flowed.
        // Dispatch one by one; React 18 auto-batches a single re-render.
        for (const a of batch) dispatch(a);
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
                case 'alert': pending.current.push({ type: 'ALERT', level: frame.level ?? 0, msg: frame.msg ?? '', ms: Date.now() }); break;
                case 'conn_state': pending.current.push({ type: 'CONN_STATE', status: frame.status ?? 'unknown' }); break;
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
