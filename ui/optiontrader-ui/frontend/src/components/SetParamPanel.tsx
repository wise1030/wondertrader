import React, { useEffect, useMemo, useState } from 'react';
import { useData } from '../context/DataContext';
import type { Snapshot } from '../types';

type Params = NonNullable<NonNullable<Snapshot['params']>>;
type Expiry = NonNullable<Snapshot['expiries']>[number];

interface RowDef {
  base: string;                // schema key ("targ_delta")
  label: string;
  min?: number;
  max?: number;
  get: (values: Params['values'], exp: Expiry | null) => number | null;
}

interface RowInst {
  def: RowDef;
  fullKey: string;             // global: base; per-expiry: "base:YYYYMM"
  exp: Expiry | null;
}

// P1 (audit 11-23): rows render from the registry schema the backend
// publishes (snapshot.params.schema).  Layout audit 11-23: TWO tables side
// by side — globals (never change) and the SELECTED month's per-expiry
// params (follows the BOARD selection; switching rows switches the table).

export const SetParamPanel: React.FC<{ selectedExpiry: number | null }> = ({ selectedExpiry }) => {
  const { snapshot, readonly, sendCommand, connectionStatus } = useData();
  const [drafts, setDrafts] = useState<Record<string, string>>({});

  const params = snapshot?.params ?? null;
  const values = params?.values ?? null;
  const connected = connectionStatus === 'connected';

  const selExpiry = snapshot?.expiries.find(e => e.expiry === selectedExpiry) ?? null;

  const globalRows = useMemo<RowDef[]>(() =>
    (params?.schema ?? []).filter(s => !s.perExpiry).map(s => ({
      base: s.key, label: s.key, min: s.min, max: s.max,
      get: v => v[s.key] ?? null,
    })), [params?.schema]);

  const expiryRowDefs = useMemo<RowDef[]>(() =>
    (params?.schema ?? []).filter(s => s.perExpiry).map(s => ({
      base: s.key, label: s.key, min: s.min, max: s.max,
      get: (_v, exp) => {
        if (!exp || !s.expField) return null;
        const f = exp as unknown as Record<string, number | null | undefined>;
        return f[s.expField] ?? null;
      },
    })), [params?.schema]);

  const instances = useMemo<RowInst[]>(() => [
    ...globalRows.map(d => ({ def: d, fullKey: d.base, exp: null })),
    ...(selExpiry ? expiryRowDefs.map(d => ({
      def: d, fullKey: `${d.base}:${selExpiry.expiry}`, exp: selExpiry,
    })) : []),
  ], [globalRows, expiryRowDefs, selExpiry]);

  // P1 fix (2026-11-19): drafts survive per-frame echoes; committing clears
  // only the committed row; a draft matching the echoed value is dropped
  // (external change, e.g. another operator / the REST entry).  Drafts are
  // keyed by the FULL key ("targ_delta:202612") so different months never
  // share an edit box state.
  useEffect(() => {
    setDrafts(d => {
      const next: Record<string, string> = { ...d };
      let dirty = false;
      for (const r of instances) {
        const v = values ? r.def.get(values, r.exp) : null;
        if (v != null && d[r.fullKey] === String(v)) { delete next[r.fullKey]; dirty = true; }
      }
      return dirty ? next : d;
    });
  }, [values, instances]);

  const apply = (r: RowInst) => {
    const draft = drafts[r.fullKey];
    const v = parseFloat(draft ?? '');
    if (!isFinite(v)) return;
    if (!connected) return;   // keep the draft on a dead socket
    const ok = sendCommand(JSON.stringify({ cmd: 'set_param', key: r.fullKey, value: v, cmdSeq: Date.now() % 100000 }));
    if (!ok) return;
    setDrafts(d => { const n = { ...d }; delete n[r.fullKey]; return n; });
  };

  const renderRow = (r: RowInst) => {
    const cur = values ? r.def.get(values, r.exp) : null;
    const draft = drafts[r.fullKey];
    // Audit 11-23: current value and editor share ONE cell — the input's
    // placeholder shows the live value; typing reveals a commit button.
    return (
      <tr key={r.fullKey} title={`key=${r.fullKey} current=${cur ?? '?'} range=[${r.def.min},${r.def.max}]`}>
        <td className="border border-gray-200 px-1 text-gray-600 font-mono whitespace-nowrap">{r.def.label}</td>
        <td className="border border-gray-200 px-1">
          <div className="flex items-center gap-1">
            <input
              className="w-20 border rounded px-1 py-0.5 font-mono"
              placeholder={cur != null ? String(cur) : '-'}
              value={draft ?? ''}
              disabled={readonly}
              onChange={e => setDrafts(d => ({ ...d, [r.fullKey]: e.target.value }))}
              onKeyDown={e => {
                if (e.key !== 'Enter' || readonly || !connected) return;
                apply(r);
              }}
            />
            {draft != null && draft !== '' && (
              <button
                className={`px-1.5 py-0.5 rounded text-white text-[10px] ${readonly || !connected ? 'bg-gray-300 cursor-not-allowed' : 'bg-blue-600 hover:bg-blue-500'}`}
                disabled={readonly || !connected}
                onClick={() => apply(r)}
              >
                set
              </button>
            )}
          </div>
        </td>
      </tr>
    );
  };

  return (
    <div className="flex items-start gap-3 text-[11px] flex-wrap">
      {/* 全局参数表 — 不随月份选择变化 */}
      <table className="border-collapse">
        <thead>
          <tr className="bg-gray-50 text-gray-500">
            <th className="border border-gray-200 px-1 text-left" colSpan={2}>全局参数 global</th>
          </tr>
          <tr className="bg-gray-50 text-gray-400 text-[9px]">
            <th className="border border-gray-200 px-1 text-left">param</th>
            <th className="border border-gray-200 px-1 text-left">value (type to edit)</th>
          </tr>
        </thead>
        <tbody>
          {globalRows.length === 0 && (
            <tr><td colSpan={2} className="border border-gray-200 px-2 py-1 text-gray-400">waiting for schema…</td></tr>
          )}
          {instances.filter(r => r.exp === null).map(renderRow)}
        </tbody>
      </table>

      {/* 月份参数表 — 与 BOARD 选择联动；未选中月份时占位 */}
      <table className="border-collapse">
        <thead>
          <tr className="bg-blue-50 text-blue-700">
            <th className="border border-gray-200 px-1 text-left" colSpan={2}>
              月份参数 expiry {selExpiry ? selExpiry.expiry : '— 在 BOARD 中选择月份'}
            </th>
          </tr>
          <tr className="bg-gray-50 text-gray-400 text-[9px]">
            <th className="border border-gray-200 px-1 text-left">param</th>
            <th className="border border-gray-200 px-1 text-left">value (type to edit)</th>
          </tr>
        </thead>
        <tbody>
          {!selExpiry && (
            <tr><td colSpan={2} className="border border-gray-200 px-2 py-1 text-gray-400">no expiry selected</td></tr>
          )}
          {instances.filter(r => r.exp !== null).map(renderRow)}
        </tbody>
      </table>

      {readonly && <span className="text-gray-400 self-center">(readonly — write gate locked)</span>}
      {!readonly && !connected && <span className="text-red-500 self-center">(disconnected)</span>}
    </div>
  );
};
