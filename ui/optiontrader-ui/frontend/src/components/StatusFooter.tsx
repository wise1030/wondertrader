import React from 'react';
import { useData } from '../context/DataContext';
import { useNowMs, liveAgeMs } from '../hooks/useNow';

// P1 UI redesign: single compact status line at the BOTTOM of the page —
// connection health lives where it is least distracting.  Everything an
// operator glances at per minute, nothing more.
export const StatusFooter: React.FC = () => {
    const { connectionStatus, snapshot, snapshotArrivalMs, sid, readonly, bridgeConn } = useData();
    const nowMs = useNowMs(1000);
    const age = liveAgeMs(snapshot?.snapshotAgeMs, snapshotArrivalMs, nowMs);
    const risk = snapshot?.risk;
    const connCls = connectionStatus === 'connected' ? 'bg-green-500'
        : connectionStatus === 'connecting' ? 'bg-yellow-500' : 'bg-red-500';
    return (
        <div className="flex items-center gap-3 px-3 py-0.5 bg-gray-800 text-gray-200 text-[10px] rounded-b">
            <span className="flex items-center gap-1">
                <span className={`w-2 h-2 rounded-full ${connCls}`} />
                <b>{connectionStatus}</b>
            </span>
            <span>age <b className={age != null && age > 3000 ? 'text-red-300' : ''}>{age != null ? `${age}ms` : '-'}</b></span>
            <span>TPS <b>{risk ? `${risk.tps}/${risk.tpsMax}` : '-'}</b></span>
            <span className="text-gray-400">sid {sid || '-'}</span>
            <span className="flex items-center gap-1">
                <span className={`w-1.5 h-1.5 rounded-full ${bridgeConn === 'connected' ? 'bg-green-400' : 'bg-red-400'}`} />
                bridge <b>{bridgeConn}</b>
            </span>
            <span className={`px-1.5 rounded font-bold ${readonly ? 'bg-gray-600' : 'bg-green-700'}`}>
                {readonly ? 'READONLY' : 'WRITE'}
            </span>
            {snapshot?.risk.limitsBreached && (
                <span className="bg-red-600 text-white px-1.5 rounded font-bold animate-pulse">LIMITS</span>
            )}
            <span className="ml-auto text-gray-500">optiontrader v2026-11 · mark=prev-settle</span>
        </div>
    );
};
