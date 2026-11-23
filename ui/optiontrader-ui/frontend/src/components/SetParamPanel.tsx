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
  // Audit 11-22: wgt_exp_ddelta was REMOVED — it is an init-time pricer
  // weight (CompositeOptionPricer config, no hot setter); the backend
  // SETPARAM whitelist rejects it and the snapshot never echoes it, so the
  // row always showed 0 and every set failed with "key not hot-adjustable".
  { key: 'max_tps', label: 'max_tps', get: p => p.max_tps, min: 1, max: 10000 },
  // Audit 11-22 (round 3): GVV blend weight — 0 = secondary curve (linear),
  // 1 = fitted main curve (gvv).  The quantbox GUI's gvv_weight knob; per-
  // expiry expiries[].volcurve_weight overrides still win for their month.
  { key: 'volcurve_weight', label: 'volcurve_weight', get: p => p.volcurve_weight ?? 0, min: 0, max: 1 },
  // Audit 11-23: quoting params (quantbox GUI parity; read at quote/fill
  // time by the pricer, so a hot set takes effect from the next refresh).
  { key: 'sticky_base', label: 'sticky_base', get: p => p.sticky_base ?? 0.5, min: 0, max: 5 },
  { key: 'improve_retreat_ratio', label: 'improve_retreat_ratio', get: p => p.improve_retreat_ratio ?? 3, min: 1, max: 10 },
  { key: 'trade_shock_ticks', label: 'trade_shock_ticks', get: p => p.trade_shock_ticks ?? 1, min: 0, max: 10 },
];

export const SetParamPanel: React.FC = () => {
  const { snapshot, readonly, sendCommand, connectionStatus } = useData();
  const [drafts, setDrafts] = useState<Record<string, string>>({});

  const params = snapshot?.params ?? null;
  const connected = connectionStatus === 'connected';
  // P1 fix (2026-11-19): the per-frame params echo used to wipe ALL drafts on
  // every snapshot — typing in any row was interrupted and unrelated rows
  // lost their edits. Drafts now survive echoes; committing clears only the
  // committed row. A draft whose value matches the echoed param is also
  // dropped (external change, e.g. another operator).
  useEffect(() => {
    setDrafts(d => {
      const next: Record<string, string> = { ...d };
      let dirty = false;
      for (const r of ROWS) {
        const v = params ? String(r.get(params)) : null;
        if (v != null && d[r.key] === v) { delete next[r.key]; dirty = true; }
      }
      return dirty ? next : d;
    });
  }, [params]);

  const apply = (key: string, value: number) => {
    // keep the draft when the line cannot be sent — do not silently clear
    // an operator's edit on a dead socket
    if (!connected) return;
    const ok = sendCommand(JSON.stringify({ cmd: 'set_param', key, value, cmdSeq: Date.now() % 100000 }));
    if (!ok) return;
    setDrafts(d => { const n = { ...d }; delete n[key]; return n; });
  };

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
                if (e.key !== 'Enter' || readonly || !connected) return;
                const v = parseFloat(draft ?? '');
                if (!isFinite(v)) return;
                apply(r.key, v);
                setDrafts(d => ({ ...d, [r.key]: '' }));
              }}
            />
            <button
              className={`px-1.5 py-0.5 rounded text-white text-[10px] ${readonly || !connected ? 'bg-gray-300 cursor-not-allowed' : 'bg-blue-600 hover:bg-blue-500'}`}
              disabled={readonly || !connected || draft == null || draft === ''}
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
      {readonly && <span className="text-gray-400">(readonly — write gate locked)</span>}
      {!readonly && !connected && <span className="text-red-500">(disconnected)</span>}
    </div>
  );
};
