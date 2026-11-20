import React, { useMemo } from 'react';
import { useData } from '../context/DataContext';
import { ConfirmButton } from './ConfirmButton';

// ---------------------------------------------------------------------------
// P1 UI redesign: EXPIRY BOARD — the single surface for per-expiry viewing
// AND control (absorbs ExpirySelector / ExpiryEnableRow / UnderlierView /
// HedgePanel).
//
//  - Row order = CONFIG order (snapshot.expiries, stable ascending) — F1-Fn
//    hotkeys bind to this order and never drift with readiness.
//  - EVERY row is selectable, including quoting=OFF months (viewing is
//    never gated by control state — the old selector disabled them).
//  - Row-level colour: red when |$Δ| > hedgeDeltaThreshold AND Γ≠0 (a
//    gamma-active expiry the hedger cannot passively neutralise), amber
//    when DTE <= 2.
//  - TOTAL row carries the portfolio hedge view (absorbed HedgePanel).
//  - Control strip (below, single instance bound to the SELECTED row):
//    quoting ON/OFF + qmode on/off/close + ALL.  Dangerous actions (OFF,
//    qmode off/close, cancel-everything) go through ConfirmButton; PANIC
//    and resume-class actions stay one-tap.
//  - readonly=true: strip renders disabled-grey with "(readonly)"; viewing
//    (selection, tooltips) always works.
// ---------------------------------------------------------------------------

const fmt = (v: number | null | undefined, d = 1) =>
  v == null || !isFinite(v) ? '-' : v.toFixed(d);

export const ExpiryBoard: React.FC<{
  selected: number | null;
  onSelect: (expiry: number) => void;
}> = ({ selected, onSelect }) => {
  const { snapshot, readonly, sendCommand } = useData();
  const expiries = useMemo(() => snapshot?.expiries ?? [], [snapshot]);

  // money delta = position delta x forward (greeks already carry the
  // contract multiplier via OptionRiskData::update)
  const rows = useMemo(() => expiries.map(e => {
    const fwd = e.fwd ?? 0;
    const d = e.greeks?.delta ?? null;
    const g = e.greeks?.gamma ?? null;
    const t = e.greeks?.theta ?? null;
    const v = e.greeks?.vega ?? null;
    const dMoney = d != null && fwd > 0 ? d * fwd : d;
    return { e, dMoney, g, t, v };
  }), [expiries]);

  const thr = snapshot?.hedgeDeltaThreshold ?? 0;

  return (
    <div className="bg-white border border-gray-200 rounded overflow-x-auto">
      <table className="border-collapse text-[11px] w-full">
        <thead>
          <tr className="bg-gray-50 text-gray-500">
            <th className="border border-gray-200 px-1 text-left">F</th>
            <th className="border border-gray-200 px-1 text-left">expiry</th>
            <th className="border border-gray-200 px-1 text-right">DTE</th>
            <th className="border border-gray-200 px-1 text-right">fwd</th>
            <th className="border border-gray-200 px-1 text-right">atmVol</th>
            <th className="border border-gray-200 px-1 text-right">$Δ</th>
            <th className="border border-gray-200 px-1 text-right">$Γ</th>
            <th className="border border-gray-200 px-1 text-right">$Θ</th>
            <th className="border border-gray-200 px-1 text-right">ν</th>
            <th className="border border-gray-200 px-1 text-right">futPos</th>
            <th className="border border-gray-200 px-1 text-center">Q</th>
            <th className="border border-gray-200 px-1 text-center">qmode</th>
            <th className="border border-gray-200 px-1 text-center">books</th>
            <th className="border border-gray-200 px-1 text-right">und mkt</th>
            <th className="border border-gray-200 px-1 text-right">und ord</th>
          </tr>
        </thead>
        <tbody>
          {rows.map(({ e, dMoney, g, t, v }, i) => {
            const isSel = e.expiry === selected;
            const und = e.und;
            const gammaActive = g != null && Math.abs(g) > 1e-9;
            const over = thr > 0 && dMoney != null && Math.abs(dMoney) > thr;
            const nearExp = e.daysToExpiry >= 0 && e.daysToExpiry <= 2;
            const rowCls = over && gammaActive ? 'bg-red-100'
              : nearExp ? 'bg-amber-50' : isSel ? 'bg-blue-50' : '';
            const undTip = und
              ? `hedge ${und.code} bid ${fmt(und.bid)} ask ${fmt(und.ask)} pos ${fmt(und.pos, 0)}`
              : 'no hedge leg';
            return (
              <tr key={e.expiry}
                onClick={() => onSelect(e.expiry)}
                className={`${rowCls} cursor-pointer hover:bg-yellow-50 ${isSel ? 'font-bold' : ''}`}
                title={`${undTip} · quoting=${e.quoting !== false} · fit=${e.fitReady} fwd=${e.fwdReady} · books ${e.booksWithBid}/${e.booksTotal}`}>
                <td className="border border-gray-200 px-1 text-gray-400">{i < 8 ? `F${i + 1}` : ''}</td>
                <td className="border border-gray-200 px-1 font-mono">{e.expiry}</td>
                <td className={`border border-gray-200 px-1 text-right ${nearExp ? 'text-red-600' : ''}`}>{e.daysToExpiry}</td>
                <td className="border border-gray-200 px-1 text-right">{fmt(e.fwd)}</td>
                <td className="border border-gray-200 px-1 text-right">{fmt(e.atmVol, 4)}</td>
                <td className="border border-gray-200 px-1 text-right">{fmt(dMoney)}</td>
                <td className="border border-gray-200 px-1 text-right">{fmt(g, 3)}</td>
                <td className="border border-gray-200 px-1 text-right">{fmt(t)}</td>
                <td className="border border-gray-200 px-1 text-right">{fmt(v)}</td>
                <td className={`border border-gray-200 px-1 text-right ${und?.pos ? 'font-bold' : ''}`}>{fmt(und?.pos, 0)}</td>
                <td className="border border-gray-200 px-1 text-center">
                  <span className={`inline-block w-2 h-2 rounded-full ${e.quoting === false ? 'bg-gray-300' : 'bg-green-500'}`}
                    title={`quoting=${e.quoting !== false}`} />
                </td>
                {/* qmode aggregation over the month's contracts — the visual
                    feedback for the qmode off/close toggles (a point of
                    confusion: off used to cancel quotes with no UI trace) */}
                {(() => {
                  const modes = new Set<number>();
                  for (const st of e.strikes) {
                    if (st.C?.qmode != null) modes.add(st.C.qmode);
                    if (st.P?.qmode != null) modes.add(st.P.qmode);
                  }
                  let cls = 'bg-gray-300', lab = 'auto';
                  if (modes.size === 1) {
                    const m = [...modes][0];
                    if (m === 1) { cls = 'bg-green-600'; lab = 'on'; }
                    else if (m === -1) { cls = 'bg-red-500'; lab = 'off'; }
                    else if (m === 2) { cls = 'bg-amber-500'; lab = 'close'; }
                    else if (m === 3) { cls = 'bg-yellow-400'; lab = 'flat'; }
                  } else if (modes.size > 1) { cls = 'bg-yellow-400'; lab = 'mix'; }
                  return (
                    <td className="border border-gray-200 px-1 text-center">
                      <span className={`inline-block px-1 rounded text-[9px] text-white ${cls}`}
                        title={`qmode aggregate over ${e.strikes.length} strikes (on=green off=red close=amber mix=yellow)`}>{lab}</span>
                    </td>
                  );
                })()}
                <td className={`border border-gray-200 px-1 text-center ${e.booksWithBid === 0 ? 'text-red-600' : ''}`}>
                  {e.booksWithBid}/{e.booksTotal}
                </td>
                {/* futures-leg MARKET bid/ask (was tooltip-only after the
                    UnderlierView fold — the operator asked for it direct) */}
                <td className="border border-gray-200 px-1 text-right font-mono"
                    title={und ? `${und.code} market` : 'no hedge leg'}>
                  {und && und.bid != null && und.ask != null
                    ? <span>{fmt(und.bid, 0)}<span className="text-gray-400">/</span>{fmt(und.ask, 0)}</span>
                    : '-'}
                </td>
                {/* our futures-leg quotes — symmetric to ordBid/ordAsk */}
                <td className="border border-gray-200 px-1 text-right font-mono"
                    title={und?.ourBid != null ? `our ${fmt(und.ourBid)}x${fmt(und.ourBidSz, 0)} / ${fmt(und.ourAsk)}x${fmt(und.ourAskSz, 0)}` : 'no live futures quote'}>
                  {und && und.ourBid != null
                    ? <span className="text-blue-700 font-bold">{fmt(und.ourBid, 0)}<span className="text-gray-400">/</span>{fmt(und.ourAsk ?? 0, 0)}</span>
                    : '-'}
                </td>
              </tr>
            );
          })}
          {/* TOTAL row — absorbed HedgePanel summary */}
          {rows.length > 1 && (() => {
            const sD = rows.reduce((a, r) => a + (r.dMoney ?? 0), 0);
            const sG = rows.reduce((a, r) => a + (r.g ?? 0), 0);
            const sT = rows.reduce((a, r) => a + (r.t ?? 0), 0);
            const sV = rows.reduce((a, r) => a + (r.v ?? 0), 0);
            return (
              <tr className="font-bold bg-gray-100">
                <td className="border border-gray-200 px-1" colSpan={5}>TOTAL</td>
                <td className="border border-gray-200 px-1 text-right">{fmt(sD)}</td>
                <td className="border border-gray-200 px-1 text-right">{fmt(sG, 3)}</td>
                <td className="border border-gray-200 px-1 text-right">{fmt(sT)}</td>
                <td className="border border-gray-200 px-1 text-right">{fmt(sV)}</td>
                <td className="border border-gray-200 px-1" colSpan={6} />
              </tr>
            );
          })()}
        </tbody>
      </table>
      <ControlStrip selected={selected} expiries={rows.map(r => r.e)} readonly={readonly} sendCommand={sendCommand} />
    </div>
  );
};

// ---------------------------------------------------------------------------
// Control strip — single instance bound to the SELECTED row.  readonly
// renders the disabled-grey state (capability visible, permission gated).
// ---------------------------------------------------------------------------
const ControlStrip: React.FC<{
  selected: number | null;
  expiries: any[];
  readonly: boolean;
  sendCommand: (line: string) => void;
}> = ({ selected, expiries, readonly, sendCommand }) => {
  const cur = expiries.find(e => e.expiry === selected) ?? null;
  const on = cur ? cur.quoting !== false : false;
  const btnBase = 'px-1.5 py-0.5 rounded text-[10px] border';

  return (
    <div className="flex items-center gap-1.5 flex-wrap px-2 py-1 border-t border-gray-200 text-[10px]">
      <span className="font-semibold text-gray-500">ctrl</span>
      {selected == null || !cur ? (
        <span className="text-gray-400">select an expiry row above to control it</span>
      ) : (
        <>
          <span className="font-mono text-gray-600">{selected}</span>
          {/* P1 UI: runtime stop/cancel is qmode off's job — the quoting
              toggle was a duplicate control surface.  EXPIRYENABLE stays in
              the backend (config enable=false cold months), surfaced here
              ONLY as the cold-month bring-up button (ON when quoting=false). */}
          {!on && (
            <button
              disabled={readonly}
              title={`hot: bring up quoting for this cold month (EXPIRYENABLE)`}
              onClick={() => sendCommand(JSON.stringify({ cmd: 'expiry_enable', expiry: selected, enable: true, cmdSeq: Date.now() % 100000 }))}
              className={`${btnBase} bg-green-700 text-white border-green-700 ${readonly ? 'opacity-40 cursor-not-allowed' : ''}`}>
              quoting ON (cold start)
            </button>
          )}
          {/* qmode off/close are dangerous (stop/cancel), on is recovery */}
          <span className="text-gray-400">qmode</span>
          <button disabled={readonly}
            onClick={() => sendCommand(JSON.stringify({ cmd: 'qmode_expiry', expiry: selected, mode: 'on', cmdSeq: Date.now() % 100000 }))}
            title="qmode on (hot)"
            className={`${btnBase} bg-white text-gray-700 border-gray-300 ${readonly ? 'opacity-40 cursor-not-allowed' : 'hover:bg-blue-50'}`}>on</button>
          <ConfirmButton label="off" confirmLabel="confirm off?"
            disabled={readonly}
            title="qmode off (hot) — pauses quoting for this month"
            className={`${btnBase} bg-white text-gray-700 border-gray-300 ${readonly ? 'opacity-40 cursor-not-allowed' : 'hover:bg-amber-50'}`}
            onConfirm={() => sendCommand(JSON.stringify({ cmd: 'qmode_expiry', expiry: selected, mode: 'off', cmdSeq: Date.now() % 100000 }))} />
          <ConfirmButton label="close" confirmLabel="confirm close?"
            disabled={readonly}
            title="qmode close (hot) — reduce-only for this month"
            className={`${btnBase} bg-white text-gray-700 border-gray-300 ${readonly ? 'opacity-40 cursor-not-allowed' : 'hover:bg-amber-50'}`}
            onConfirm={() => sendCommand(JSON.stringify({ cmd: 'qmode_expiry', expiry: selected, mode: 'close', cmdSeq: Date.now() % 100000 }))} />
          {/* ALL scope */}
          <span className="text-gray-400 ml-2">ALL:</span>
          {(['on', 'off', 'close'] as const).map(m => (
            <ConfirmButton key={m} label={m}
              confirmLabel={m === 'on' ? 'confirm on?' : `confirm ALL ${m}?`}
              disabled={readonly}
              title={`qmode ${m} for EVERY month (hot)${m === 'on' ? '' : ' — dangerous'}`}
              className={`${btnBase} ${readonly ? 'opacity-40 cursor-not-allowed' : 'bg-white text-gray-700 border-gray-300 hover:bg-blue-50'}`}
              window={m === 'on' ? 0 : 3000}
              onConfirm={() => {
                // ALL mode applies per-expiry for every configured series
                for (const r of expiries)
                  sendCommand(JSON.stringify({ cmd: 'qmode_expiry', expiry: r.e.expiry, mode: m, cmdSeq: (Date.now() + r.e.expiry) % 100000000 }));
              }} />
          ))}
          {readonly && <span className="text-gray-400">(readonly)</span>}
        </>
      )}
    </div>
  );
};
