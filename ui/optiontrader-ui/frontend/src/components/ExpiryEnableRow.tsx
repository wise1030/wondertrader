import React from 'react';
import { useData } from '../context/DataContext';

// P1 follow-up: per-expiry quoting on/off (hot command EXPIRYENABLE).
// Click toggles the quoting switch for that series — hot-off stages
// cancels for its live quotes; hot-on brings a cold series up fully.
export const ExpiryEnableRow: React.FC<{ readonly: boolean; sendCommand: (line: string) => void }> =
({ readonly, sendCommand }) => {
  const { snapshot } = useData();
  const expiries = snapshot?.expiries ?? [];
  if (expiries.length === 0) return null;

  const toggle = (expiry: number, cur: boolean) => {
    if (readonly) return;
    sendCommand(JSON.stringify({ cmd: 'expiry_enable', expiry, enable: !cur, cmdSeq: Date.now() % 100000 }));
  };

  return (
    <div className="flex items-center gap-1 flex-wrap text-[11px]">
      <span className="font-semibold text-gray-500 mr-1">quoting</span>
      {expiries.map(e => {
        const on = e.quoting !== false;
        return (
          <button key={e.expiry}
            disabled={readonly}
            onClick={() => toggle(e.expiry, on)}
            title={`${e.expiry} quoting=${on ? 'ON' : 'OFF'} — click to ${on ? 'stop' : 'start'} (hot)`}
            className={`px-1.5 py-0.5 rounded text-[10px] font-mono border
              ${readonly ? 'opacity-40 cursor-not-allowed' : 'cursor-pointer'}
              ${on ? 'bg-green-700 text-white border-green-700' : 'bg-gray-200 text-gray-500 border-gray-300'}`}>
            {e.expiry} {on ? 'ON' : 'OFF'}
          </button>
        );
      })}
      {readonly && <span className="text-[10px] text-gray-400">(readonly)</span>}
    </div>
  );
};
