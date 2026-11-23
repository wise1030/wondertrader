import React, { useEffect, useMemo, useState } from 'react';
import { useData } from '../context/DataContext';
import type { Snapshot } from '../types';

type Params = NonNullable<NonNullable<Snapshot['params']>>;

interface Row {
  key: string;                 // SETPARAM key (global or "name:YYYYMM")
  label: string;
  get: (values: Params['values'], exps: Snapshot['expiries']) => number | null;
  min?: number;
  max?: number;
  group?: string;              // UI grouping label (expiry month)
}

// P1 (audit 11-23): the panel renders ENTIRELY from the registry schema the
// backend publishes in snapshot.params.schema — global entries become static
// rows, perExpiry entries are crossed with snapshot.expiries.  Adding a key
// on the backend (buildParamRegistry) needs NO frontend change.  The 5 alpha
// signal weights keep their backend hot channel but are operator-hidden
// because the backend does not publish them in the schema.

export const SetParamPanel: React.FC = () => {
  const { snapshot, readonly, sendCommand, connectionStatus } = useData();
  const [drafts, setDrafts] = useState<Record<string, string>>({});

  const params = snapshot?.params ?? null;
  const values = params?.values ?? null;
  const connected = connectionStatus === 'connected';

  const rows = useMemo<Row[]>(() => {
    const rs: Row[] = [];
    for (const se of params?.schema ?? []) {
      if (!se.perExpiry) {
        rs.push({
          key: se.key, label: se.key, min: se.min, max: se.max,
          get: v => v[se.key] ?? null,
        });
      } else {
        for (const e of snapshot?.expiries ?? []) {
          rs.push({
            key: `${se.key}:${e.expiry}`,
            label: `${se.key}:${String(e.expiry).slice(2)}`,
            group: String(e.expiry),
            min: se.min, max: se.max,
            get: (_v, exps) => {
              const ee = exps?.find(x => x.expiry === e.expiry);
              if (!ee || !se.expField) return null;
              const f = ee as unknown as Record<string, number | null | undefined>;
              return f[se.expField!] ?? null;
            },
          });
        }
      }
    }
    return rs;
  }, [params?.schema, snapshot?.expiries]);

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
        const v = values ? r.get(values, snapshot?.expiries ?? []) : null;
        if (v != null && d[r.key] === String(v)) { delete next[r.key]; dirty = true; }
      }
      return dirty ? next : d;
    });
  }, [values, rows, snapshot?.expiries]);

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
      {rows.length === 0 && <span className="text-gray-400">(schema unavailable — waiting for snapshot)</span>}
      {rows.map(r => {
        const cur = values ? r.get(values, snapshot?.expiries ?? []) : null;
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
