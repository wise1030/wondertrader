import React, { useEffect, useMemo, useState } from 'react';
import { useData } from '../context/DataContext';
import type { Snapshot } from '../types';
import { LineChart } from './AnalysisTab';
import { blackIV, fetchSeries, quantiles, BAND_PS, BAND_NAMES } from './analysisShared';

const volFmt = (v: number) => (v * 100).toFixed(1) + '%';

// ---------------------------------------------------------------------------
// 6.1 smile error band: market bid/ask IV band (Black-76 inversion) around
// the fitted theo curve; optional multi-expiry DTE-gradient overlay.
// ---------------------------------------------------------------------------
export const SmileBand: React.FC<{ snapshot: Snapshot | null; expiry: number | null }> =
({ snapshot, expiry }) => {
  const [overlay, setOverlay] = useState(false);
  const exp = snapshot?.expiries.find(e => e.expiry === expiry) ?? null;

  const data = useMemo(() => {
    if (!exp) return null;
    const strikes = [...exp.strikes].sort((a, b) => a.k - b.k);
    const F = exp.fwd ?? 0;
    const dte = exp.daysToExpiry ?? 0;
    const atm = exp.atmVol ?? null;
    const bidIv = (side: 'C' | 'P') => strikes.map(s => {
      const px = side === 'C' ? s.C.bid : s.P.bid;
      return px != null && px > 0 ? blackIV(px, F, s.k, dte, side === 'C') : null;
    });
    const askIv = (side: 'C' | 'P') => strikes.map(s => {
      const px = side === 'C' ? s.C.ask : s.P.ask;
      return px != null && px > 0 ? blackIV(px, F, s.k, dte, side === 'C') : null;
    });
    const theo = (side: 'C' | 'P') => strikes.map(s => {
      const v = side === 'C' ? s.C.vshift : s.P.vshift;
      return v != null && atm != null ? atm + v : null;
    });
    // DTE-gradient overlay of the other expiries' fitted curves
    const others = overlay && snapshot
      ? snapshot.expiries
          .filter(e => e.expiry !== expiry && e.atmVol != null && e.daysToExpiry >= 0)
          .sort((a, b) => b.daysToExpiry - a.daysToExpiry)   // far = stronger
          .slice(0, 4)
          .map((e, i, arr) => ({
            name: `${e.expiry} (${e.daysToExpiry}d)`,
            color: `rgba(37, 99, 235, ${0.15 + 0.6 * (i / Math.max(1, arr.length - 1))})`,
            points: [...e.strikes].sort((a, b) => a.k - b.k).map(s =>
              s.C.vshift != null ? e.atmVol! + s.C.vshift : null),
            dashed: true,
          }))
      : [];
    return { strikes, atm, bidC: bidIv('C'), askC: askIv('C'), theoC: theo('C'), others };
  }, [exp, overlay, snapshot, expiry]);

  if (!data) return <div className="text-gray-400 text-[11px] p-4">no expiry data</div>;

  return (
    <div>
      <LineChart
        yFmt={volFmt}
        series={[
          { name: 'bid iv', color: '#bfdbfe', dashed: true, points: data.bidC },
          { name: 'ask iv', color: '#bfdbfe', dashed: true, points: data.askC },
          { name: 'C theo', color: '#2563eb', points: data.theoC },
          ...(data.atm != null
            ? [{ name: 'atm', color: '#111827', dashed: true, points: data.strikes.map(() => data.atm!) }] : []),
          ...data.others,
        ]}
      />
      <div className="flex items-center gap-2 mt-0.5">
        <span className="text-[10px] text-gray-400">
          band = market bid/ask inverted (Black-76, F=fwd) · theo = atm + vshift
        </span>
        <label className="text-[10px] text-gray-500 flex items-center gap-1">
          <input type="checkbox" checked={overlay} onChange={e => setOverlay(e.target.checked)} />
          DTE overlay
        </label>
      </div>
    </div>
  );
};

// ---------------------------------------------------------------------------
// 6.2 vol cone: percentile band of the atmvol history (multi-day via
// /api/series?tdate=recent) + today's intraday trace + current marker.
// ---------------------------------------------------------------------------
export const VolCone: React.FC<{ tdate: number }> = ({ tdate }) => {
  const [resp, setResp] = useState<{ rows: number[][]; days: number } | null>(null);
  useEffect(() => {
    let live = true;
    // Audit 11-22: the panel claimed multi-day history but never requested
    // it — it fetched a single numeric tdate, so the "cone" was today's
    // intraday band only, and on a non-trading day it was empty even with
    // weeks of files on disk.  'recent' is the intended 6.2 mode.
    const load = () => fetchSeries('vol', 'recent')
      .then(r => { if (live && r) setResp({ rows: r.rows, days: r.days ?? 0 }); });
    load();
    const timer = setInterval(load, 60000);
    return () => { live = false; clearInterval(timer); };
  }, [tdate]);

  const cone = useMemo(() => {
    if (!resp || resp.rows.length === 0) return null;
    // column layout: ts, expiry, atmvol, fwd, dte
    const byExp = new Map<number, number[]>();
    for (const r of resp.rows) {
      const list = byExp.get(r[1]) ?? [];
      list.push(r[2]);
      byExp.set(r[1], list);
    }
    return [...byExp.entries()].map(([expiry, vals]) => ({
      expiry, band: quantiles(vals, BAND_PS), n: vals.length,
      cur: vals[vals.length - 1],
    }));
  }, [resp]);

  if (!resp || !cone) return <div className="text-gray-400 text-[11px] p-4">no vol history yet (recorder writes 1 row/min once the session runs)</div>;

  return (
    <div>
      <table className="border-collapse text-[10px] w-full">
        <thead>
          <tr className="bg-gray-50 text-gray-500">
            <th className="border border-gray-200 px-1 text-left">expiry</th>
            {BAND_NAMES.map(n => <th key={n} className="border border-gray-200 px-1 text-right">{n}</th>)}
            <th className="border border-gray-200 px-1 text-right">now</th>
            <th className="border border-gray-200 px-1 text-right">n</th>
          </tr>
        </thead>
        <tbody>
          {cone.map(c => (
            <tr key={c.expiry}>
              <td className="border border-gray-200 px-1 font-mono">{c.expiry}</td>
              {c.band.map((v, i) => (
                <td key={i} className={`border border-gray-200 px-1 text-right ${i === 3 ? 'font-bold' : ''}`}>
                  {isFinite(v) ? volFmt(v) : '-'}
                </td>
              ))}
              <td className="border border-gray-200 px-1 text-right font-bold text-blue-700">{volFmt(c.cur)}</td>
              <td className="border border-gray-200 px-1 text-right text-gray-400">{c.n}</td>
            </tr>
          ))}
        </tbody>
      </table>
      <div className="text-[10px] text-gray-400 mt-0.5">
        atmvol history bands (MIN/P10/P25/P50/P75/P90/MAX over recorded minutes)
        {resp.days > 0 ? ` · ${resp.days} day file(s)` : ''}{tdate > 0 ? ` · today=${tdate}` : ''}
      </div>
    </div>
  );
};

// ---------------------------------------------------------------------------
// 6.3 attribution: per-fill edge capture (px vs issue theo) + cumulative
// toggle; portfolio greek decomposition from the pf series.
// ---------------------------------------------------------------------------
export const Attribution: React.FC<{ tdate: number }> = ({ tdate }) => {
  const { fills, snapshot } = useData();
  const [cum, setCum] = useState(true);
  const [pf, setPf] = useState<number[][] | null>(null);

  useEffect(() => {
    let live = true;
    const load = () => fetchSeries('pf', tdate > 0 ? tdate : 0)
      .then(r => { if (live) setPf(r?.rows ?? null); });
    load();
    const timer = setInterval(load, 60000);
    return () => { live = false; clearInterval(timer); };
  }, [tdate]);

  // per-fill edge (spread capture vs issue-time theo); sign by side.
  // Audit fix (11-20): px/theo are per-lot premium quotes — money needs the
  // contract multiplier from snapshot.meta.contracts (cu volscale=5 was
  // under-reporting edges 5x).
  const edges = useMemo(() => fills
    .filter(f => f.ctx?.theo != null)
    .map(f => {
      const mult = snapshot?.meta?.contracts?.[f.code]?.mult ?? 1;
      const raw = (f.buy ? -1 : 1) * (f.px - f.ctx!.theo!) * f.qty * mult;
      return { t: f.t, code: f.code, edge: raw, buy: f.buy };
    }), [fills, snapshot]);

  const cumCurve = useMemo(() => {
    let s = 0;
    return edges.map(e => (s += e.edge));
  }, [edges]);

  // portfolio greek decomposition from the minute series
  const decomp = useMemo(() => {
    if (!pf || pf.length < 2) return null;
    // cols: ts,delta,gamma,vega,theta,pnl,spot,atmvol
    const dPnL: number[] = [], gPnL: number[] = [], vPnL: number[] = [], tPnL: number[] = [];
    for (let i = 1; i < pf.length; ++i) {
      const p = pf[i - 1], q = pf[i];
      const dS = q[6] - p[6], dv = q[7] - p[7], dtsec = q[0] - p[0];
      dPnL.push(p[1] * dS);
      gPnL.push(0.5 * p[2] * dS * dS);
      vPnL.push(p[3] * dv);
      // Audit fix (11-20): engine theta (BlackCalc::thetaPerDay) is NEGATIVE
      // for time decay, so pnl += theta * dt_days directly — the extra -1
      // sign flip turned every decay into a phantom gain.
      tPnL.push(p[4] * (dtsec / 86400.0));
    }
    const cumul = (a: number[]) => { let s = 0; return a.map(v => (s += v)); };
    return {
      d: cumul(dPnL), g: cumul(gPnL), v: cumul(vPnL), t: cumul(tPnL),
      actual: pf.slice(1).map(r => r[5] - pf[0][5]),
    };
  }, [pf]);

  return (
    <div className="grid grid-cols-1 xl:grid-cols-2 gap-2">
      <div>
        <div className="flex items-center gap-2 mb-0.5">
          <span className="text-[10px] font-bold text-gray-600">fill edge capture (px vs issue theo)</span>
          <label className="text-[10px] text-gray-500 flex items-center gap-1">
            <input type="checkbox" checked={cum} onChange={e => setCum(e.target.checked)} /> cum
          </label>
        </div>
        {edges.length > 0 ? (
          <LineChart height={140} yFmt={v => v.toFixed(1)}
            series={[{
              name: cum ? 'cum edge' : 'edge',
              color: '#059669',
              points: cum ? cumCurve : edges.map(e => e.edge),
            }]} />
        ) : <div className="text-gray-400 text-[11px]">no fills with issue-time theo (enable fillAttribution)</div>}
        <div className="text-[10px] text-gray-400">fills={edges.length} · today {cumCurve.length ? cumCurve[cumCurve.length - 1].toFixed(1) : '0'}</div>
      </div>
      <div>
        <div className="text-[10px] font-bold text-gray-600 mb-0.5">portfolio greek decomposition (minute series)</div>
        {decomp ? (
          <LineChart height={140} yFmt={v => v.toFixed(0)}
            series={[
              { name: 'Δ', color: '#2563eb', points: decomp.d },
              { name: 'Γ', color: '#ea580c', points: decomp.g },
              { name: 'ν', color: '#7c3aed', points: decomp.v },
              { name: 'Θ', color: '#9ca3af', dashed: true, points: decomp.t },
              { name: 'pnl', color: '#111827', points: decomp.actual },
            ]} />
        ) : <div className="text-gray-400 text-[11px]">no pfseries yet (1 row/min once running)</div>}
      </div>
    </div>
  );
};

// ---------------------------------------------------------------------------
// 6.4 prediction accuracy chips
// ---------------------------------------------------------------------------
export const PredChips: React.FC = () => {
  const { snapshot } = useData();
  const p = snapshot?.pred;
  if (!p || (p.resolved === 0 && p.scanResolved === 0))
    return <span className="text-[10px] text-gray-400">no predictions resolved yet</span>;
  const chip = (label: string, v: string, color: string) => (
    <span className={`px-1.5 py-0.5 border rounded text-[10px] font-bold ${color}`}>{label} {v}</span>
  );
  return (
    <span className="flex items-center gap-1 flex-wrap">
      {p.resolved > 0 && <>
        {chip('fwd30s hit', p.hitRate != null ? p.hitRate.toFixed(0) + '%' : '-', 'bg-blue-50 text-blue-700 border-blue-200')}
        {chip('mae', p.mae != null ? p.mae.toFixed(2) : '-', 'bg-gray-50 text-gray-600 border-gray-200')}
        <span className="text-[10px] text-gray-400">n={p.resolved}</span>
      </>}
      {p.scanResolved > 0 && <>
        {chip('scan hit', p.scanHitRate != null ? p.scanHitRate.toFixed(0) + '%' : '-', 'bg-purple-50 text-purple-700 border-purple-200')}
        {chip('IC', p.scanIc != null ? p.scanIc.toFixed(2) : '-', 'bg-purple-50 text-purple-700 border-purple-200')}
        <span className="text-[10px] text-gray-400">n={p.scanResolved}</span>
      </>}
    </span>
  );
};
