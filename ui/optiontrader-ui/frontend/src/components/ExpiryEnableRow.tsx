import React from 'react';
import { useData } from '../context/DataContext';

// P1 follow-up: per-expiry controls (hot, scoped to ONE series):
//   quoting ON/OFF  — EXPIRYENABLE (stages cancels when going off)
//   qmode on/off/close — QMODEE (quote mode of the series' contracts)
export const ExpiryEnableRow: React.FC<{ readonly: boolean; sendCommand: (line: string) => void }> =
({ readonly, sendCommand }) => {
  const { snapshot } = useData();
  const expiries = snapshot?.expiries ?? [];
  if (expiries.length === 0) return null;

  const setQMode = (expiry: number, mode: 'on' | 'off' | 'close') => {
    if (readonly) return;
    sendCommand(JSON.stringify({ cmd: 'qmode_expiry', expiry, mode, cmdSeq: Date.now() % 100000 }));
  };
  const toggleQuoting = (expiry: number, cur: boolean) => {
    if (readonly) return;
    sendCommand(JSON.stringify({ cmd: 'expiry_enable', expiry, enable: !cur, cmdSeq: Date.now() % 100000 }));
  };

  return (
    <div className="flex items-center gap-2 flex-wrap text-[11px]">
      <span className="font-semibold text-gray-500 mr-1">expiry ctrl</span>
      {expiries.map(e => {
        const on = e.quoting !== false;
        return (
          <span key={e.expiry} className="flex items-center gap-0.5 border border-gray-200 rounded px-1 py-0.5">
            <span className="font-mono text-[10px] text-gray-600 mr-0.5">{e.expiry}</span>
            <button disabled={readonly}
              onClick={() => toggleQuoting(e.expiry, on)}
              title={`${e.expiry} quoting ${on ? 'ON' : 'OFF'} — click to ${on ? 'stop quoting (cancels)' : 'start quoting'} (hot)`}
              className={`px-1 rounded text-[10px] font-bold border
                ${readonly ? 'opacity-40 cursor-not-allowed' : 'cursor-pointer'}
                ${on ? 'bg-green-700 text-white border-green-700' : 'bg-gray-200 text-gray-500 border-gray-300'}`}>
              {on ? 'ON' : 'OFF'}
            </button>
            {(['on', 'off', 'close'] as const).map(m => (
              <button key={m} disabled={readonly}
                onClick={() => setQMode(e.expiry, m)}
                title={`${e.expiry} qmode ${m} (hot)`}
                className={`px-1 rounded text-[9px] border
                  ${readonly ? 'opacity-40 cursor-not-allowed' : 'hover:bg-blue-50'}
                  bg-white text-gray-600 border-gray-300`}>
                {m}
              </button>
            ))}
          </span>
        );
      })}
      {readonly && <span className="text-[10px] text-gray-400">(readonly)</span>}
    </div>
  );
};
