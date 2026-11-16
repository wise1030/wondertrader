import React, { useEffect, useState } from 'react';
import { useData } from '../context/DataContext';

const STATE_COLORS: Record<string, string> = {
    Normal: 'bg-green-600',
    Recovering: 'bg-yellow-500',
    Panic: 'bg-red-600',
    SessionStopped: 'bg-gray-500',
    Disabled: 'bg-gray-400',
};

const Lamp: React.FC<{ label: string; on: boolean; okColor?: string }> = ({ label, on, okColor = 'bg-green-500' }) => (
    <span className="flex items-center gap-1">
        <span className={`w-2.5 h-2.5 rounded-full ${on ? okColor : 'bg-gray-300'}`} />
        <span className="text-[10px] text-gray-200">{label}</span>
    </span>
);

export const StatusBanner: React.FC = () => {
    const { snapshot, connectionStatus } = useData();
    const [nowSec, setNowSec] = useState(() => Date.now() / 1000);
    useEffect(() => {
        const t = setInterval(() => setNowSec(Date.now() / 1000), 1000);
        return () => clearInterval(t);
    }, []);

    const st = snapshot?.state;
    const trading = st?.trading ?? '-';
    const color = STATE_COLORS[trading] ?? 'bg-gray-600';

    const connColor = connectionStatus === 'connected' ? 'bg-green-500'
        : connectionStatus === 'connecting' ? 'bg-yellow-500' : 'bg-red-500';

    return (
        <div className={`${color} text-white px-3 py-1.5 flex items-center gap-4 rounded`}>
            <div className="flex items-center gap-2">
                <span className={`w-2.5 h-2.5 rounded-full ${connColor}`} />
                <span className="font-bold text-sm tracking-wide">{trading}</span>
                {st?.reason ? <span className="text-[10px] opacity-80">({st.reason})</span> : null}
            </div>
            <Lamp label="channel" on={!!st?.channelReady} okColor="bg-green-300" />
            <Lamp label="underlying" on={!!st?.underlyingReady} okColor="bg-green-300" />
            {st?.panicked ? <span className="text-xs font-bold bg-black/30 px-1.5 rounded">PANICKED</span> : null}
            <span className="text-[10px] opacity-80">tdate {st?.tdate ?? '-'}</span>
            {snapshot?.risk.sessionStopInSec != null && (
                <span className="text-[10px] bg-black/25 px-1.5 rounded">
                    session stop in {Math.floor(snapshot.risk.sessionStopInSec / 60)}:
                    {String(snapshot.risk.sessionStopInSec % 60).padStart(2, '0')}
                </span>
            )}
            <span className="ml-auto text-[10px] opacity-70">{nowSec && `age ${snapshot?.snapshotAgeMs ?? '?'}ms`}</span>
        </div>
    );
};
