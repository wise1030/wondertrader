import React from 'react';
import { useData } from '../context/DataContext';
import { ConfirmButton } from './ConfirmButton';

interface Props {
  onNotify: (msg: string, type: 'info' | 'error') => void;
}

export const CommandPanel: React.FC<Props> = ({ onNotify }) => {
  const { msgLogs, readonly, sid, connectionStatus, refresh, sendCommand } = useData();

  // Audit 11-22: every write command now reports two failure modes instead
  // of pretending success — readonly gate and a dead WebSocket.
  const send = (cmd: string) => {
    if (readonly) { onNotify(`${cmd} is gated — readonly mode`, 'info'); return; }
    const ok = sendCommand(JSON.stringify({ cmd, cmdSeq: Date.now() % 100000 }));
    if (!ok) onNotify('not connected — command NOT sent', 'error');
  };

  return (
    <div className="border-t border-gray-300 bg-white px-3 py-1.5 flex flex-col gap-1">
      <div className="flex items-center gap-2 text-[11px]">
        <span className="font-bold text-gray-600">Command</span>
        <span className="text-gray-400">sid {sid || '-'}</span>
        <span className={`px-1.5 rounded text-[10px] font-bold ${readonly ? 'bg-gray-200 text-gray-600' : 'bg-green-200 text-green-800'}`}>
          {readonly ? 'READONLY' : 'WRITE'}
        </span>
        <button
          className="px-2 py-0.5 bg-blue-600 text-white rounded hover:bg-blue-500"
          onClick={refresh}
          disabled={connectionStatus !== 'connected'}
        >
          Refresh
        </button>
        <span className="text-gray-300">|</span>
        {(['STOP', 'RESUME', 'PANIC'] as const).map(c => (
          // P1 UI redesign: STOP gets a two-tap confirm (full strategy halt);
          // PANIC stays zero-friction (kill switch); RESUME is recovery.
          c === 'STOP' ? (
            <ConfirmButton key={c} label="STOP" confirmLabel="confirm STOP?"
              disabled={readonly}
              title="halt the whole strategy (cancels all quotes)"
              className={`px-2 py-0.5 rounded text-white bg-gray-600 hover:bg-gray-500 ${readonly ? 'cursor-not-allowed opacity-40' : ''}`}
              onConfirm={() => send('stop')} />
          ) : (
            <button key={c}
              title={c === 'PANIC' ? 'kill switch — cancels and halts (write mode required)' : `${c} (write mode required)`}
              className={`px-2 py-0.5 rounded text-white ${c === 'PANIC' ? 'bg-red-600 hover:bg-red-500' : 'bg-gray-600 hover:bg-gray-500'} ${readonly ? 'cursor-not-allowed opacity-40' : ''}`}
              onClick={() => send(c.toLowerCase())}
            >
              {c}
            </button>
          )
        ))}
        {/* P1 UI redesign: grid-wide qmode moved to the EXPIRY BOARD ctrl
            strip (ALL scope) — one control surface per scope. */}
      </div>
      <div className="h-20 overflow-y-auto bg-gray-50 border border-gray-200 rounded px-2 py-0.5 text-[10px] font-mono">
        {msgLogs.slice().reverse().map((l, i) => (
          <div key={i} className={l.type === 'error' || l.type === 'reject' ? 'text-red-600' : 'text-gray-600'}>
            [{l.time}] {l.type}: {l.content}
          </div>
        ))}
        {msgLogs.length === 0 && <div className="text-gray-400">no messages</div>}
      </div>
    </div>
  );
};
