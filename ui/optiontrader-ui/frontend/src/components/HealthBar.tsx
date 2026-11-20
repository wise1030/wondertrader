import React from 'react';
import { useData } from '../context/DataContext';
import { useNowMs, liveAgeMs } from '../hooks/useNow';

// P1 UI redesign: 3-item condensed health strip (conn / snapshot age / TPS).
// The full 12-item health grid lives in the Analysis tab now.
export const HealthBar: React.FC = () => {
    const { snapshot, snapshotArrivalMs, connectionStatus } = useData();
    const nowMs = useNowMs(1000);
    const age = liveAgeMs(snapshot?.snapshotAgeMs, snapshotArrivalMs, nowMs);
    const risk = snapshot?.risk;
    const bad = connectionStatus !== 'connected' || (age != null && age > 3000);
    return (
        <div className={`flex items-center gap-4 px-3 py-0.5 rounded text-[11px] border ${bad ? 'bg-red-50 border-red-200' : 'bg-white border-gray-200'}`}>
            <span className={`font-bold ${connectionStatus === 'connected' ? 'text-green-700' : 'text-red-600'}`}>{connectionStatus}</span>
            <span className="text-gray-500">age <b className={age != null && age > 3000 ? 'text-red-600' : ''}>{age != null ? `${age}ms` : '-'}</b></span>
            <span className="text-gray-500">TPS <b>{risk ? `${risk.tps}/${risk.tpsMax}` : '-'}</b></span>
            {snapshot?.risk.limitsBreached && (
                <span className="bg-red-600 text-white px-2 rounded font-bold animate-pulse">LIMITS BREACHED</span>
            )}
        </div>
    );
};
