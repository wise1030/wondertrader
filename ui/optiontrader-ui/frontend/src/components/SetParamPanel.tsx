import React, { useEffect, useMemo, useState } from 'react';
import { useData } from '../context/DataContext';
import type { Snapshot } from '../types';

type Params = NonNullable<NonNullable<Snapshot['params']>>;

interface Row {
  key: string;                 // SETPARAM key (global or "name:YYYYMM")
  label: string;
  get: (p: Params, expiries: Snapshot['expiries']) => number | null;
  min?: number;
  max?: number;
  group?: string;              // UI grouping label (e.g. expiry month)
}

// Audit 11-23 (quantbox GUI parity): the operator-facing hot params.
// The 5 alpha signal weights keep their BACKEND hot channel (sync_param /
// SETPARAM) but are intentionally NOT in this panel — they are quant-style
// tunables, not operator controls.  Per-expiry rows (targ_*/sprd_*:*)
// mirror quantbox's dotted expiry namespace and are generated dynamically
// from the snapshot below.
const STATIC_ROWS: Row[] = [
  { key: 'max_tps', label: 'max_tps', get: p => p.max_tps, min: 1, max: 10000 },
  // Audit 11-22 (round 3): GVV blend weight — 0 = secondary curve (linear),
  // 1 = fitted main curve (gvv).  The quantbox GUI's gvv_weight knob; per-
  // expiry expiries[].volcurve_weight overrides still win for their month.
  { key: 'volcurve_weight', label: 'volcurve_weight', get: p => p.volcurve_weight ?? 0, min: 0, max: 1 },
  // Audit 11-23: quoting params (read at quote/fill time by the pricer, so a
  // hot set takes effect from the next refresh).
  { key: 'sticky_base', label: 'sticky_base', get: p => p.sticky_base ?? 0.5, min: 0, max: 5 },
  { key: 'improve_retreat_ratio', label: 'improve_retreat_ratio', get: p => p.improve_retreat_ratio ?? 3, min: 1, max: 10 },
  { key: 'trade_shock_ticks', label: 'trade_shock_ticks', get: p => p.trade_shock_ticks ?? 1, min: 0, max: 10 },
];

const EXPIRY_PARAM_DEFS = [
  { name: 'targ_delta', get: (e: NonNullable<Snapshot['expiries']>[number]) => e.targDelta ?? null, min: -5000, max: 5000 },
  { name: 'targ_vega', get: (e: NonNullable<Snapshot['expiries']>[number]) => e.targVega ?? null, min: -5000, max: 5000 },
  { name: 'sprd_fwd', get: (e: NonNullable<Snapshot['expiries']>[number]) => e.sprdFwd ?? null, min: 0, max: 1 },
  { name: 'sprd_atmvol', get: (e: NonNullable<Snapshot['expiries']>[number]) => e.sprdAtmvol ?? null, min: 0, max: 1 },
  { name: 'sprd_corr', get: (e: NonNullable<Snapshot['expiries']>[number]) => e.sprdCorr ?? null, min: -1, max: 1 },
];

export const SetParamPanel: React.FC = () => {
  const { snapshot, readonly, sendCommand, connectionStatus } = useData();
  const [drafts, setDrafts] = useState<Record<string, string>>({});

  const params = snapshot?.params ?? null;
  const connected = connectionStatus === 'connected';

  // Static globals + one row per (param, expiry) — mirrors the quantbox
  // per-expiry parameter table without hand-written rows.
  const rows = useMemo<Row[]>(() => {
    const rs: Row[] = [...STATIC_ROWS];
    for (const e of snapshot?.expiries ?? []) {
      for (const d of EXPIRY_PARAM_DEFS) {
        rs.push({
          key: `${d.name}:${e.expiry}`,
          label: `${d.name}:${String(e.expiry).slice(2)}`,
          group: String(e.expiry),
          min: d.min,
          max: d.max,
          get: (_p, exps) => {
            const ee = exps.find(x => x.expiry === e.expiry);
            return ee ? d.get(ee) : null;
          },
        });
      }
    }
    return rs;
  }, [snapshot?.expiries]);

  // P1 fix (2026-11-19): the per-frame params echo used to wipe ALL drafts on
  // every snapshot — typing in any row was interrupted and unrelated rows
  // lost their edits. Drafts now survive echoes; committing clears only the
  // committed row. A draft whose value matches the echoed param is also
  // dropped (external change, e.g. another operator).
  useEffect(() => {
    setDrafts(d => {
      const next: Record<string, string> = { ...d };
      let dirty = false;
      for (const r of rows) {
        const v = params ? r.get(params, snapshot?.expiries ?? []) : null;
        if (v != null && d[r.key] === String(v)) { delete next[r.key]; dirty = true; }
      }
      return dirty ? next : d;
    });
  }, [params, rows, snapshot?.expiries]);

  const apply = (key: string, value: number) => {
    // keep the draft when the line cannot be sent — do not silently clear
    // an operator's edit on a dead socket
    if (!connected) return;
    const ok = sendCommand(JSON.stringify({ cmd: 'set_param', key, value, cmdSeq: Date.now() % 100000 }));
    if (!ok) return;
    setDrafts(d => { const n = { ...d }; delete n[key]; return n; });
  };

  let lastGroup: string | undefined;
  return (
    <div className="flex items-center gap-3 flex-wrap text-[11px]">
      <span className="font-semibold text-gray-500">Hot params</span>
      {rows.map(r => {
        const cur = params ? r.get(params, snapshot?.expiries ?? []) : null;
        const draft = drafts[r.key];
        const header = r.group !== lastGroup ? r.group : undefined;
        lastGroup = r.group;
        return (
          <React.Fragment key={r.key}>
            {header && <span className="text-gray-400 font-semibold">·{header}</span>}
            <label className="flex items-center gap-1" title={`key=${r.key} current=${cur ?? '?'} range=[${r.min},${r.max}]`}>
              <span className="text-gray-500">{r.label}</span>
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
          </React.Fragment>
        );
      })}
      {readonly && <span className="text-gray-400">(readonly — write gate locked)</span>}
      {!readonly && !connected && <span className="text-red-500">(disconnected)</span>}
    </div>
  );
};
