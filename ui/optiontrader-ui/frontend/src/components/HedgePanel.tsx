import React, { useMemo, useState } from 'react';
import { useData } from '../context/DataContext';

const fmt = (v: number | null | undefined, d = 0) =>
  v == null || !isFinite(v) ? '-' : v.toFixed(d);

// ---------------------------------------------------------------------------
// P1 batch-5.1: hedge panel — per-expiry money greeks with hedge-signal
// colouring, ported from the quantbox hedge-panel review:
//   $Δ $Γ $Θ $V per expiry, subtotal rows (options / hedge futures / total),
//   row red when |$Δ| > hedgeDeltaThreshold AND Γ ≠ 0 (gamma-active expiry the
//   hedger cannot passively neutralise), near-expiry rows amber.
// ---------------------------------------------------------------------------
interface Row {
  key: string;
  label: string;
  d: number | null;
  g: number | null;
  t: number | null;
  v: number | null;
  pos: number | null;
  cls: string;
  indent?: boolean;
}

export const HedgePanel: React.FC = () => {
  const { snapshot } = useData();
  const [open, setOpen] = useState(false);

  const rows = useMemo<Row[]>(() => {
    const exps = snapshot?.expiries ?? [];
    const thr = snapshot?.hedgeDeltaThreshold ?? 0;
    const out: Row[] = [];
    let sD = 0, sG = 0, sT = 0, sV = 0;
    for (const e of exps) {
      // multiplier: futures contract multiplier of the hedge underlying
      const mult = 1;   // greeks already position-aggregated; $ via fwd below
      const fwd = e.fwd ?? 0;
      const d = e.greeks?.delta ?? null;
      const g = e.greeks?.gamma ?? null;
      const t = e.greeks?.theta ?? null;
      const v = e.greeks?.vega ?? null;
      // $-ize: delta by forward, the rest straight (position greeks)
      const dMoney = d != null && fwd > 0 ? d * fwd * mult : d;
      const gMoney = g != null ? g * mult : null;
      const tMoney = t != null ? t * mult : null;
      const vMoney = v != null ? v * mult : null;
      if (dMoney) sD += dMoney;
      if (gMoney) sG += gMoney;
      if (tMoney) sT += tMoney;
      if (vMoney) sV += vMoney;

      const gammaActive = g != null && Math.abs(g) > 1e-9;
      const over = thr > 0 && dMoney != null && Math.abs(dMoney) > thr;
      const nearExp = e.daysToExpiry <= 2;
      const cls = over && gammaActive ? 'bg-red-100'
                : nearExp ? 'bg-amber-50'
                : '';
      const sig = over && gammaActive ? '⚠' : '';
      out.push({
        key: String(e.expiry), label: `${e.expiry}${sig}`,
        d: dMoney, g: gMoney, t: tMoney, v: vMoney,
        pos: e.und?.pos ?? null, cls,
      });
    }
    if (out.length > 1)
      out.push({ key: 'total', label: 'TOTAL', d: sD, g: sG, t: sT, v: sV, pos: null, cls: 'font-bold bg-gray-100' });
    return out;
  }, [snapshot]);

  if (!open)
    return (
      <button onClick={() => setOpen(true)}
        className="px-2 py-0.5 bg-white border border-gray-200 rounded text-[10px] text-gray-500 hover:bg-gray-50">
        hedge panel ▸
      </button>
    );

  return (
    <div className="bg-white border border-gray-200 rounded p-1">
      <div className="flex items-center justify-between mb-0.5">
        <span className="text-[10px] font-bold text-gray-600">
          Hedge panel{snapshot?.hedgeDeltaThreshold ? ` (|Δ|>${fmt(snapshot.hedgeDeltaThreshold)} & Γ≠0 → red)` : ''}
        </span>
        <button onClick={() => setOpen(false)} className="text-[10px] text-gray-400 hover:text-gray-600">▾ collapse</button>
      </div>
      <table className="border-collapse text-[10px] w-full">
        <thead>
          <tr className="bg-gray-50 text-gray-500">
            <th className="border border-gray-200 px-1 text-left">expiry</th>
            <th className="border border-gray-200 px-1 text-right">$Δ</th>
            <th className="border border-gray-200 px-1 text-right">$Γ</th>
            <th className="border border-gray-200 px-1 text-right">$Θ</th>
            <th className="border border-gray-200 px-1 text-right">$V</th>
            <th className="border border-gray-200 px-1 text-right">futPos</th>
          </tr>
        </thead>
        <tbody>
          {rows.map(r => (
            <tr key={r.key} className={r.cls}>
              <td className="border border-gray-200 px-1 font-mono">{r.label}</td>
              <td className="border border-gray-200 px-1 text-right">{fmt(r.d)}</td>
              <td className="border border-gray-200 px-1 text-right">{fmt(r.g)}</td>
              <td className="border border-gray-200 px-1 text-right">{fmt(r.t)}</td>
              <td className="border border-gray-200 px-1 text-right">{fmt(r.v)}</td>
              <td className="border border-gray-200 px-1 text-right">{fmt(r.pos)}</td>
            </tr>
          ))}
          {rows.length === 0 && <tr><td className="text-gray-400 px-1" colSpan={6}>no expiries</td></tr>}
        </tbody>
      </table>
    </div>
  );
};
