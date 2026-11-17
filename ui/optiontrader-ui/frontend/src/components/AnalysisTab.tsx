import React, { useEffect, useMemo, useRef, useState } from 'react';
import { useData } from '../context/DataContext';
import type { Snapshot } from '../types';

const MAX_POINTS = 600;   // 10 minutes at 1Hz

// ---------------------------------------------------------------------------
// tiny SVG line chart helper
// ---------------------------------------------------------------------------
interface Series {
  name: string;
  color: string;
  dashed?: boolean;
  points: (number | null)[];
}

const LineChart: React.FC<{
  series: Series[];
  height?: number;
  yFmt?: (v: number) => string;
  yPadRatio?: number;
}> = ({ series, height = 220, yFmt, yPadRatio = 0.08 }) => {
  const W = 640, H = height, ML = 52, MR = 16, MT = 8, MB = 22;
  const all = series.flatMap(s => s.points.filter((p): p is number => p != null));
  if (all.length === 0)
    return <div className="text-gray-400 text-[11px] p-4">no data yet</div>;

  let yMin = Math.min(...all), yMax = Math.max(...all);
  if (yMin === yMax) { yMin -= 1; yMax += 1; }
  const pad = (yMax - yMin) * yPadRatio;
  yMin -= pad; yMax += pad;

  const n = Math.max(...series.map(s => s.points.length));
  const x = (i: number) => ML + (n <= 1 ? 0 : (i / (n - 1)) * (W - ML - MR));
  const y = (v: number) => MT + (1 - (v - yMin) / (yMax - yMin)) * (H - MT - MB);

  // Null (gap) breakpoints split the polyline: an uninitialized stretch must
  // NOT be bridged by a straight line (audit batch B, M7 — misleading signal).
  const path = (pts: (number | null)[]) => {
    const segs: string[] = [];
    let cur = '';
    pts.forEach((p, i) => {
      if (p == null) {
        if (cur) { segs.push(cur); cur = ''; }
        return;
      }
      cur += `${cur ? 'L' : 'M'}${x(i).toFixed(1)},${y(p).toFixed(1)}`;
    });
    if (cur) segs.push(cur);
    return segs.join(' ');
  };

  const yMid = (yMin + yMax) / 2;
  const fmt = yFmt ?? ((v: number) => v.toFixed(2));

  return (
    <svg viewBox={`0 0 ${W} ${H}`} className="w-full bg-white border border-gray-200 rounded">
      <line x1={ML} y1={MT} x2={ML} y2={H - MB} stroke="#e5e7eb" />
      <line x1={ML} y1={H - MB} x2={W - MR} y2={H - MB} stroke="#e5e7eb" />
      {[yMin, yMid, yMax].map((v, i) => (
        <g key={i}>
          <line x1={ML} y1={y(v)} x2={W - MR} y2={y(v)} stroke="#f3f4f6" />
          <text x={ML - 4} y={y(v) + 3} textAnchor="end" fontSize={9} fill="#9ca3af">{fmt(v)}</text>
        </g>
      ))}
      {series.map(s => (
        <path key={s.name} d={path(s.points)} fill="none" stroke={s.color} strokeWidth={1.4}
          strokeDasharray={s.dashed ? '4 3' : undefined} />
      ))}
      <g>
        {series.map((s, i) => (
          <text key={s.name} x={ML + 8 + i * 110} y={H - 6} fontSize={10} fill={s.color}>
            {s.dashed ? '--- ' : '— '}{s.name}
          </text>
        ))}
      </g>
    </svg>
  );
};

// ---------------------------------------------------------------------------
// Vol smile: strike -> iv (market) and theo vol (atmVol + vshift), per side
// ---------------------------------------------------------------------------
const VolSmile: React.FC<{ snapshot: Snapshot | null; expiry: number | null }> = ({ snapshot, expiry }) => {
  const exp = snapshot?.expiries.find(e => e.expiry === expiry) ?? null;
  const data = useMemo(() => {
    if (!exp) return null;
    const strikes = [...exp.strikes].sort((a, b) => a.k - b.k);
    const atm = exp.atmVol ?? null;
    const cIv = strikes.map(s => s.C.iv);
    const pIv = strikes.map(s => s.P.iv);
    const cTheo = strikes.map(s => (s.C.vshift != null && atm != null) ? atm + s.C.vshift : null);
    const pTheo = strikes.map(s => (s.P.vshift != null && atm != null) ? atm + s.P.vshift : null);
    return { strikes, atm, cIv, pIv, cTheo, pTheo };
  }, [exp]);

  if (!data) return <div className="text-gray-400 text-[11px] p-4">no expiry data</div>;
  const ks = data.strikes.map(s => s.k);
  const volFmt = (v: number) => (v * 100).toFixed(1) + '%';

  return (
    <div>
      <LineChart
        yFmt={volFmt}
        series={[
          { name: 'C iv', color: '#2563eb', points: data.cIv },
          { name: 'P iv', color: '#ea580c', points: data.pIv },
          { name: 'C theo', color: '#93c5fd', dashed: true, points: data.cTheo },
          { name: 'P theo', color: '#fdba74', dashed: true, points: data.pTheo },
          ...(data.atm != null ? [{ name: 'atm', color: '#111827', dashed: true, points: data.strikes.map(() => data.atm!) }] : []),
        ]}
      />
      <div className="text-[10px] text-gray-400 mt-0.5">
        strikes {ks.length > 0 ? `${ks[0]} .. ${ks[ks.length - 1]}` : '-'} · atmVol {data.atm != null ? volFmt(data.atm) : '-'} · theo = atm + vshift
      </div>
    </div>
  );
};

// ---------------------------------------------------------------------------
// Time-series rings fed by the snapshot stream
// ---------------------------------------------------------------------------
function useRing<T>(pick: (s: Snapshot) => T): T[] {
  const { snapshot } = useData();
  const ring = useRef<T[]>([]);
  const [, force] = useState(0);
  const lastSeq = useRef(0);
  useEffect(() => {
    if (!snapshot || snapshot.seq === lastSeq.current) return;
    lastSeq.current = snapshot.seq;
    ring.current = [...ring.current, pick(snapshot)].slice(-MAX_POINTS);
    force(x => x + 1);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [snapshot]);
  return ring.current;
}

const PnlChart: React.FC = () => {
  const pts = useRing(s => ({ cur: s.pnl.cur, realized: s.pnl.realized }));
  return (
    <div>
      <LineChart
        series={[
          { name: 'cur', color: '#059669', points: pts.map(p => p.cur) },
          { name: 'realized', color: '#9ca3af', dashed: true, points: pts.map(p => p.realized) },
        ]}
        yFmt={v => v.toFixed(0)}
      />
      <div className="text-[10px] text-gray-400 mt-0.5">portfolio PnL, last {pts.length} snapshots</div>
    </div>
  );
};

const LatencyChart: React.FC = () => {
  const pts = useRing(s => ({ p50: s.perf.batchP50, p95: s.perf.batchP95, p99: s.perf.batchP99 }));
  return (
    <div>
      <LineChart
        series={[
          { name: 'p50', color: '#059669', points: pts.map(p => p.p50) },
          { name: 'p95', color: '#d97706', points: pts.map(p => p.p95) },
          { name: 'p99', color: '#dc2626', points: pts.map(p => p.p99) },
        ]}
        yFmt={v => v.toFixed(0) + 'µs'}
      />
      <div className="text-[10px] text-gray-400 mt-0.5">AEP batch latency, last {pts.length} snapshots</div>
    </div>
  );
};

export const AnalysisTab: React.FC<{ selectedExpiry: number | null }> = ({ selectedExpiry }) => {
  const { snapshot } = useData();
  return (
    <div className="h-full overflow-auto p-2 grid grid-cols-1 xl:grid-cols-2 gap-2">
      <div className="bg-white border border-gray-200 rounded p-2">
        <div className="text-xs font-bold text-gray-600 mb-1">Vol smile {selectedExpiry ?? ''}</div>
        <VolSmile snapshot={snapshot} expiry={selectedExpiry} />
      </div>
      <div className="bg-white border border-gray-200 rounded p-2">
        <div className="text-xs font-bold text-gray-600 mb-1">PnL</div>
        <PnlChart />
      </div>
      <div className="bg-white border border-gray-200 rounded p-2 xl:col-span-2">
        <div className="text-xs font-bold text-gray-600 mb-1">Batch latency</div>
        <LatencyChart />
      </div>
    </div>
  );
};
