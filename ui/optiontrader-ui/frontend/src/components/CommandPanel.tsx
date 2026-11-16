import React from 'react';
import { useData } from '../context/DataContext';
import { SetParamPanel } from './SetParamPanel';

interface Props {
  onNotify: (msg: string, type: 'info' | 'error') => void;
}

export const CommandPanel: React.FC<Props> = ({ onNotify }) => {
  const { msgLogs, readonly, sid, connectionStatus, refresh, sendCommand } = useData();

  const p2 = (what: string) => onNotify(`${what} is a P2 command — readonly mode`, 'info');

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
          <button
            key={c}
            className={`px-2 py-0.5 rounded text-white ${c === 'PANIC' ? 'bg-red-600 hover:bg-red-500' : 'bg-gray-600 hover:bg-gray-500'} ${readonly ? 'cursor-not-allowed opacity-40' : ''}`}
            onClick={() => readonly ? p2(c) : sendCommand(JSON.stringify({ cmd: c.toLowerCase(), cmdSeq: Date.now() % 100000 }))}
          >
            {c}
          </button>
        ))}
        {(['on', 'off', 'close'] as const).map(m => (
          <button
            key={m}
            className={`px-2 py-0.5 rounded bg-gray-600 hover:bg-gray-500 text-white ${readonly ? 'cursor-not-allowed opacity-40' : ''}`}
            onClick={() => readonly ? p2(`qmode ${m}`) : sendCommand(JSON.stringify({ cmd: 'qmode', mode: m, cmdSeq: Date.now() % 100000 }))}
          >
            qmode {m}
          </button>
        ))}
      </div>
      <SetParamPanel />
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
