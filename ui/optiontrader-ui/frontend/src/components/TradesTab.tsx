import React, { useEffect, useMemo, useState } from 'react';
import { useData } from '../context/DataContext';
import { OrdersTable } from './OrdersTable';

// P1 batch-4.3: order-source attribution.  0/undefined=Unknown 1=MM 2=Hedge 3=Scanner.
const TAG_LABEL = ['?', 'MM', 'H', 'SC'] as const;
const TAG_CLASS = [
  'bg-gray-100 text-gray-500 border-gray-200',
  'bg-blue-50 text-blue-700 border-blue-200',
  'bg-amber-50 text-amber-700 border-amber-200',
  'bg-purple-50 text-purple-700 border-purple-200',
] as const;

export const tagLabel = (t?: number) => TAG_LABEL[(t ?? 0) & 3];
export const tagClass = (t?: number) => TAG_CLASS[(t ?? 0) & 3];

export const TradesTab: React.FC<{
  onNotify?: (msg: string, type: 'info' | 'error') => void;
}> = ({ onNotify }) => {
  const { fills, snapshot } = useData();
  // Audit 11-22: the tag filter used to reset on every remount (tab switch).
  // Session-scoped persistence keeps the operator's view stable.
  const [tagFilter, setTagFilter] = useState<number>(() => {
    const v = Number(sessionStorage.getItem('opt_tagFilter'));
    return Number.isFinite(v) ? v : -1;
  });   // -1 = all
  useEffect(() => {
    try { sessionStorage.setItem('opt_tagFilter', String(tagFilter)); } catch { /* private mode */ }
  }, [tagFilter]);
  const recent = useMemo(
    () => [...fills].reverse().filter(f => tagFilter < 0 || (f.tag ?? 0) === tagFilter).slice(0, 500),
    [fills, tagFilter]);

  // Per-source summary chips from the snapshot risk buckets.
  const fillsByTag = snapshot?.risk?.fillsByTag;
  const liveByTag = snapshot?.risk?.liveByTag;
  const chip = (tag: number, label: string) => (
    <button key={tag} onClick={() => setTagFilter(tagFilter === tag ? -1 : tag)}
      className={`px-1.5 py-0.5 border rounded text-[10px] font-bold ${tagFilter === tag ? tagClass(tag) : 'bg-white text-gray-400 border-gray-200'}`}
      title={`filter: ${label}`}>
      {label} <span className="font-normal">F{fillsByTag?.[tag] ?? 0}/L{liveByTag?.[tag] ?? 0}</span>
    </button>
  );

  return (
    <div className="flex flex-col h-full gap-2 p-2 overflow-auto">
      <div className="bg-white border border-gray-200 rounded p-2">
        <OrdersTable onNotify={onNotify} />
      </div>
      <div className="bg-white border border-gray-200 rounded p-2">
        <div className="flex items-center gap-1 mb-1 flex-wrap">
          <span className="text-xs font-bold text-gray-600 mr-1">Recent fills (newest first, max 500)</span>
          {chip(1, 'MM')}{chip(2, 'Hedge')}{chip(3, 'Scanner')}{chip(0, 'Unknown')}
          {tagFilter >= 0 && <button onClick={() => setTagFilter(-1)}
            className="px-1.5 py-0.5 border rounded text-[10px] text-gray-500 border-gray-300">clear</button>}
        </div>
        <div className="grid grid-cols-2 md:grid-cols-3 lg:grid-cols-4 gap-1">
          {recent.map((f, i) => (
            <div key={i} className={`border rounded px-1.5 py-0.5 text-[10px] flex justify-between gap-2 ${f.buy ? 'border-green-200 bg-green-50' : 'border-red-200 bg-red-50'}`}>
              <span className="font-mono truncate">{f.code}</span>
              <span className={`px-0.5 border rounded text-[9px] ${tagClass(f.tag)}`}>{tagLabel(f.tag)}</span>
              <span className={f.buy ? 'text-green-700 font-bold' : 'text-red-700 font-bold'}>{f.buy ? 'B' : 'S'}</span>
              <span>{f.qty}@{f.px}</span>
            </div>
          ))}
          {recent.length === 0 && <div className="text-gray-400 text-[11px]">{tagFilter >= 0 ? 'no fills for this source' : 'no fills yet'}</div>}
        </div>
      </div>
    </div>
  );
};
