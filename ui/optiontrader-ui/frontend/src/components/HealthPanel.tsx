import React from 'react';
import { useData } from '../context/DataContext';
import { useNowMs, liveAgeMs } from '../hooks/useNow';

const fmt = (v: number | null | undefined, d = 0) => (v == null ? '-' : v.toFixed(d));

export const HealthPanel: React.FC = () => {
    const { snapshot, snapshotArrivalMs, connectionStatus } = useData();
    const nowMs = useNowMs(1000);
    const perf = snapshot?.perf;
    const risk = snapshot?.risk;

    // Live age (audit batch B, H1) — >3s means the snapshot stream stalled.
    const age = liveAgeMs(snapshot?.snapshotAgeMs, snapshotArrivalMs, nowMs);

    const Item: React.FC<{ k: string; v: React.ReactNode; warn?: boolean }> = ({ k, v, warn }) => (
        <div className="flex justify-between gap-2">
            <span className="text-gray-500">{k}</span>
            <span className={warn ? 'text-red-600 font-bold' : 'font-mono'}>{v}</span>
        </div>
    );

    return (
        <div className="grid grid-cols-3 gap-x-6 gap-y-0.5 px-3 py-1.5 bg-white border border-gray-200 rounded text-[11px]">
            <div>
                <Item k="conn" v={connectionStatus} warn={connectionStatus !== 'connected'} />
                <Item k="snapshot age" v={age != null ? `${age}ms` : '-'} warn={age != null && age > 3000} />
                <Item k="batch p50/p95/p99 µs" v={`${fmt(perf?.batchP50)} / ${fmt(perf?.batchP95)} / ${fmt(perf?.batchP99)}`} />
                <Item k="aep events" v={fmt(perf?.aepEvents)} />
            </div>
            <div>
                <Item k="queue drops" v={fmt(perf?.queueDrops)} warn={!!perf && perf.queueDrops > 0} />
                <Item k="md overflow / trader depth" v={`${fmt(perf?.mdOverflow)} / ${fmt(perf?.traderDepth)}`} warn={!!perf && perf.mdOverflow > 0} />
                <Item k="ui event drops" v={fmt(perf?.eventDrops)} warn={!!perf && perf.eventDrops > 0} />
                <Item k="ui coalesced" v={fmt(perf?.snapshotCoalesced)} warn={!!perf && perf.snapshotCoalesced > 10} />
            </div>
            <div>
                <Item k="TPS" v={`${risk?.tps ?? '-'}/${risk?.tpsMax ?? '-'}`} />
                <Item k="orders/fills/cxl/rej" v={`${risk?.ordersSent ?? '-'}/${risk?.fills ?? '-'}/${risk?.cancels ?? '-'}/${risk?.rejects ?? '-'}`} />
                <Item k="fillRatio / cancelRatio" v={`${fmt(risk?.fillRatio, 3)} / ${fmt(risk?.cancelRatio, 3)}`} />
            </div>
        </div>
    );
};
