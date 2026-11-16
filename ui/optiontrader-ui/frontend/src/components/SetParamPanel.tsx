import React, { useEffect, useState } from 'react';
import { useData } from '../context/DataContext';
import type { Snapshot } from '../types';

type Params = NonNullable<NonNullable<Snapshot['params']>>;

interface Row {
  key: keyof Params;
  label: string;
  get: (p: Params) => number;
  min?: number;
  max?: number;
}

const ROWS: Row[] = [
  { key: 'wgt_vegaflow', label: 'wgt_vegaflow', get: p => p.wgt_vegaflow, min: 0, max: 10 },
  { key: 'wgt_frontfut_skew', label: 'wgt_frontfut_skew', get: p => p.wgt_frontfut_skew, min: 0, max: 10 },
  { key: 'wgt_deltaflow', label: 'wgt_deltaflow', get: p => p.wgt_deltaflow, min: 0, max: 10 },
  { key: 'wgt_atmsig', label: 'wgt_atmsig', get: p => p.wgt_atmsig, min: 0, max: 10 },
  { key: 'wgt_rollema', label: 'wgt_rollema', get: p => p.wgt_rollema, min: 0, max: 10 },
  { key: 'max_tps', label: 'max_tps', get: p => p.max_tps, min: 1, max: 10000 },
];

export const SetParamPanel: React.FC = () => {
  const { snapshot, readonly, sendCommand } = useData();
  const [drafts, setDrafts] = useState<Record<string, string>>({});

  const params = snapshot?.params ?? null;
  useEffect(() => setDrafts({}), [params?.wgt_vegaflow, params?.wgt_frontfut_skew,
    params?.wgt_deltaflow, params?.wgt_atmsig, params?.wgt_rollema, params?.max_tps]);

  const apply = (key: string, value: number) =>
    sendCommand(JSON.stringify({ cmd: 'set_param', key, value, cmdSeq: Date.now() % 100000 }));

  return (
    <div className="flex items-center gap-3 flex-wrap text-[11px]">
      <span className="font-semibold text-gray-500">Hot params</span>
      {ROWS.map(r => {
        const cur = params ? r.get(params) : null;
        const draft = drafts[r.key];
        return (
          <label key={r.key} className="flex items-center gap-1" title={`current=${cur ?? '?'} range=[${r.min},${r.max}]`}>
            <span className="text-gray-500">{r.label.replace('wgt_', 'w_')}</span>
            <input
              className="w-16 border rounded px-1 py-0.5 font-mono"
              placeholder={cur != null ? String(cur) : '-'}
              value={draft ?? ''}
              disabled={readonly}
              onChange={e => setDrafts(d => ({ ...d, [r.key]: e.target.value }))}
              onKeyDown={e => {
                if (e.key !== 'Enter' || readonly) return;
                const v = parseFloat(draft ?? '');
                if (!isFinite(v)) return;
                apply(r.key, v);
                setDrafts(d => ({ ...d, [r.key]: '' }));
              }}
            />
            <button
              className={`px-1.5 py-0.5 rounded text-white text-[10px] ${readonly ? 'bg-gray-300 cursor-not-allowed' : 'bg-blue-600 hover:bg-blue-500'}`}
              disabled={readonly || draft == null || draft === ''}
              onClick={() => {
                const v = parseFloat(draft ?? '');
                if (!isFinite(v)) return;
                apply(r.key, v);
                setDrafts(d => ({ ...d, [r.key]: '' }));
              }}
            >
              set
            </button>
          </label>
        );
      })}
      {readonly && <span className="text-gray-400">(readonly — P2 gate locked)</span>}
    </div>
  );
};
