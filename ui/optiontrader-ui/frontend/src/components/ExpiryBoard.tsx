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
//  - Row-level colour: red when |Δ| (raw, same units as the configured
//    threshold) > hedgeDeltaThreshold AND Γ≠0 (a gamma-active expiry the
//    hedger cannot passively neutralise), amber when DTE <= 2.
//  - TOTAL row carries the portfolio hedge view (absorbed HedgePanel).
//  - Control strip (below, single instance bound to the SELECTED row):
//    quoting ON/OFF + qmode on/off/close (per-expiry; the old ALL scope was
//    removed).  Dangerous actions go through ConfirmButton; recovery-class
//    actions stay one-tap.
//  - readonly=true: strip renders disabled-grey with "(readonly)"; viewing
//    (selection, tooltips) always works.
// ---------------------------------------------------------------------------

const fmt = (v: number | null | undefined, d = 1) =>
  v == null || !isFinite(v) ? '-' : v.toFixed(d);

export const ExpiryBoard: React.FC<{
  selected: number | null;
  onSelect: (expiry: number) => void;
  onNotify?: (msg: string, type: 'info' | 'error') => void;
}> = ({ selected, onSelect, onNotify }) => {
  const { snapshot, readonly, sendCommand } = useData();
  const expiries = useMemo(() => snapshot?.expiries ?? [], [snapshot]);

  // delta (position-weighted by OptionRiskData) is in UNDERLYING units
  // (tonnes for cu); money delta = delta x forward.  The hedge threshold is
  // configured in the SAME delta units (threshold_delta: 30 = 30 t), so the
  // breach rule must compare raw delta, NOT money delta.
  const rows = useMemo(() => expiries.map(e => {
    const fwd = e.fwd ?? 0;
    const d = e.greeks?.delta ?? null;
    const g = e.greeks?.gamma ?? null;
    const t = e.greeks?.theta ?? null;
    const v = e.greeks?.vega ?? null;
    const dMoney = d != null && fwd > 0 ? d * fwd : d;
    return { e, d, dMoney, g, t, v };
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
            <th className="border border-gray-200 px-1 text-right" title="Δ × forward = CNY notional (delta itself is in underlying units)">$Δ</th>
            <th className="border border-gray-200 px-1 text-right" title="position-weighted gamma (raw, not dollar gamma)">Γ</th>
            <th className="border border-gray-200 px-1 text-right" title="position-weighted theta (CNY/day)">Θ</th>
            <th className="border border-gray-200 px-1 text-right" title="position-weighted vega">ν</th>
            <th className="border border-gray-200 px-1 text-right" title="hedge futures position (lots)">futPos</th>
            <th className="border border-gray-200 px-1 text-center">Q</th>
            <th className="border border-gray-200 px-1 text-center">qmode</th>
            <th className="border border-gray-200 px-1 text-center">books</th>
          </tr>
        </thead>
        <tbody>
          {rows.map(({ e, d, dMoney, g, t, v }, i) => {
            const isSel = e.expiry === selected;
            const und = e.und;
            const gammaActive = g != null && Math.abs(g) > 1e-9;
            // Audit 11-22 (round 2): the engine's per-expiry hedge gate
            // compares |option delta + hedge-leg delta| (OptionExpiryGreeks
            // ::totalDelta, i.e. checkMulti) against the threshold — the UI
            // must use the same quantity, else a fully-hedged month shows
            // red.  undDelta = hedge lots x contract size (underlying units).
            const legD = und?.undDelta ?? 0;
            const dTot = (d ?? 0) + legD;
            const over = thr > 0 && Math.abs(dTot) > thr;
            const nearExp = e.daysToExpiry >= 0 && e.daysToExpiry <= 2;
            const rowCls = over && gammaActive ? 'bg-red-100'
              : isSel ? 'bg-blue-100' : nearExp ? 'bg-amber-50' : '';
            const booksBad = e.quoting !== false && e.booksWithBid === 0;
            const undTip = und
              ? `hedge ${und.code} bid ${fmt(und.bid)} ask ${fmt(und.ask)} pos ${fmt(und.pos, 0)} Δleg ${fmt(und.undDelta, 1)}`
              : 'no hedge leg';
            return (
              <tr key={e.expiry}
                onClick={() => onSelect(e.expiry)}
                className={`${rowCls} cursor-pointer hover:bg-yellow-50 ${isSel ? 'font-bold' : ''}`}
                title={`${undTip} · Δopt ${fmt(d, 2)} Δtotal ${fmt(dTot, 2)} thr ${fmt(thr, 1)} · quoting=${e.quoting !== false ? 'ON' : 'OFF'} · fit=${e.fitReady} fwd=${e.fwdReady} · books ${e.booksWithBid}/${e.booksTotal}`}>
                <td className="border border-gray-200 px-1 text-gray-400">{i < 8 ? `F${i + 1}` : ''}</td>
                <td className="border border-gray-200 px-1 font-mono">{e.expiry}</td>
                <td className={`border border-gray-200 px-1 text-right ${nearExp ? 'text-red-600' : ''}`}>{e.daysToExpiry}</td>
                <td className="border border-gray-200 px-1 text-right">{fmt(e.fwd)}</td>
                <td className="border border-gray-200 px-1 text-right">{fmt(e.atmVol, 4)}</td>
                <td className="border border-gray-200 px-1 text-right"
                    title={`Δ ${fmt(d, 2)} underlying units × fwd ${fmt(e.fwd)} = ${fmt(dMoney)} CNY`}>
                  {fmt(dMoney)}
                </td>
                <td className="border border-gray-200 px-1 text-right">{fmt(g, 3)}</td>
                <td className="border border-gray-200 px-1 text-right">{fmt(t)}</td>
                <td className="border border-gray-200 px-1 text-right">{fmt(v)}</td>
                <td className={`border border-gray-200 px-1 text-right ${und?.pos ? 'font-bold' : ''}`}
                    title="hedge futures position for this expiry (lots)">{fmt(und?.pos, 0)}</td>
                <td className="border border-gray-200 px-1 text-center">
                  <span className={`inline-block w-2 h-2 rounded-full ${e.quoting === false ? 'bg-gray-300' : 'bg-green-500'}`}
                    title={`quoting ${e.quoting !== false ? 'ON — participates in market making' : 'OFF — excluded from market making'}`} />
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
                  const name = (m: number) => m === 1 ? 'on' : m === -1 ? 'off'
                    : m === 2 ? 'close' : m === 3 ? 'flat' : m === 0 ? 'auto' : String(m);
                  let cls = 'bg-gray-300 text-gray-900', lab = 'auto';
                  if (modes.size === 0) { cls = 'bg-gray-200 text-gray-500'; lab = 'no data'; }
                  else if (modes.size === 1) {
                    const m = [...modes][0];
                    if (m === 1) { cls = 'bg-green-600 text-white'; lab = 'on'; }
                    else if (m === -1) { cls = 'bg-red-500 text-white'; lab = 'off'; }
                    else if (m === 2) { cls = 'bg-amber-500 text-white'; lab = 'close'; }
                    else if (m === 3) { cls = 'bg-yellow-400 text-gray-900'; lab = 'flat'; }
                  } else { cls = 'bg-yellow-400 text-gray-900'; lab = 'mix'; }
                  return (
                    <td className="border border-gray-200 px-1 text-center">
                      <span className={`inline-block px-1 rounded text-[9px] ${cls}`}
                        title={`qmode over ${e.strikes.length} strikes: ${[...modes].map(name).join(', ') || 'no contracts'} (on=green off=red close=amber)`}>{lab}</span>
                    </td>
                  );
                })()}
                <td className={`border border-gray-200 px-1 text-center ${booksBad ? 'text-red-600 font-bold' : ''}`}
                    title={booksBad ? 'quoting is ON but no strike has a bid — feed/grid problem' : 'strikes with a market bid / total'}>
                  {e.booksWithBid}/{e.booksTotal}
                </td>
              </tr>
            );
          })}
          {rows.length === 0 && (
            <tr>
              <td colSpan={13} className="border border-gray-200 px-2 py-3 text-center text-gray-400">
                no expiry data — market closed / session not started (or the feed has not produced a grid yet)
              </td>
            </tr>
          )}
          {/* TOTAL row — absorbed HedgePanel summary */}
          {rows.length > 1 && (() => {
            const sD = rows.reduce((a, r) => a + (r.dMoney ?? 0), 0);
            const sDelta = rows.reduce((a, r) => a + (r.d ?? 0), 0);
            const sG = rows.reduce((a, r) => a + (r.g ?? 0), 0);
            const sT = rows.reduce((a, r) => a + (r.t ?? 0), 0);
            const sV = rows.reduce((a, r) => a + (r.v ?? 0), 0);
            // TOTAL breach uses pfDelta (option + futures-hedge delta — the
            // exact quantity the hedger compares with the threshold), with
            // the option-only sum as fallback for pre-round-2 snapshots.
            const pfD = snapshot?.greeks?.pfDelta ?? sDelta;
            const pfG = snapshot?.greeks?.gamma ?? sG;
            const totalOver = thr > 0 && Math.abs(pfD) > thr && Math.abs(pfG) > 1e-9;
            return (
              <tr className={`font-bold ${totalOver ? 'bg-red-100' : 'bg-gray-100'}`}
                  title={`portfolio Δ ${fmt(pfD, 2)} (option ${fmt(sDelta, 2)} + hedge) vs threshold ${fmt(thr, 1)}`}>
                <td className="border border-gray-200 px-1" colSpan={5}>TOTAL</td>
                <td className="border border-gray-200 px-1 text-right">{fmt(sD)}</td>
                <td className="border border-gray-200 px-1 text-right">{fmt(sG, 3)}</td>
                <td className="border border-gray-200 px-1 text-right">{fmt(sT)}</td>
                <td className="border border-gray-200 px-1 text-right">{fmt(sV)}</td>
                <td className="border border-gray-200 px-1" colSpan={4} />
              </tr>
            );
          })()}
        </tbody>
      </table>
      <ControlStrip selected={selected} expiries={rows.map(r => r.e)} readonly={readonly}
        sendCommand={sendCommand} thr={thr} onNotify={onNotify} />
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
  sendCommand: (line: string) => boolean;
  thr: number;
  onNotify?: (msg: string, type: 'info' | 'error') => void;
}> = ({ selected, expiries, readonly, sendCommand, thr, onNotify }) => {
  const { connectionStatus } = useData();
  const connected = connectionStatus === 'connected';
  const cur = expiries.find(e => e.expiry === selected) ?? null;
  const on = cur ? cur.quoting !== false : false;
  const btnBase = 'px-1.5 py-0.5 rounded text-[10px] border';
  const disabled = readonly || !connected;

  // Audit 11-22 (round 2): the BOARD commands went straight to sendCommand
  // with no readonly/disconnected handling — the widest blast-radius strip
  // on the page was the only one that could "succeed" silently on a dead
  // socket.  Same contract as Ticket/OrdersTable: report, never pretend.
  const send = (line: string, what: string) => {
    if (readonly) { onNotify?.(`${what} gated — readonly mode`, 'info'); return; }
    if (!connected) { onNotify?.(`not connected — ${what} NOT sent`, 'error'); return; }
    if (!sendCommand(line)) onNotify?.(`not connected — ${what} NOT sent`, 'error');
  };

  return (
    <div className="flex items-center gap-1.5 flex-wrap px-2 py-1 border-t border-gray-200 text-[10px]">
      <span className="font-semibold text-gray-500">ctrl</span>
      {selected == null || !cur ? (
        <span className="text-gray-400">select an expiry row above to control it</span>
      ) : (
        <>
          <span className="font-mono text-gray-600">{selected}</span>
          {/* Operator's semantics (final): quoting = WHETHER this month
              participates in market making (pricer enable); qmode = the
              quote MODE while participating (on bilateral / off paused /
              close reduce-only).  Both buttons stay. */}
          <ConfirmButton
            key={`quote:${selected}:${on}`}
            label={on ? 'quoting OFF' : 'quoting ON'}
            confirmLabel={on ? 'confirm exit MM?' : 'confirm ON?'}
            disabled={disabled}
            title={`hot EXPIRYENABLE: ${on ? 'this month EXITS market making (quotes cancelled, none generated)' : 'this month JOINS market making'}${connected ? '' : ' (disconnected)'}`}
            className={`${btnBase} ${disabled ? 'opacity-40 cursor-not-allowed' : on ? 'bg-gray-200 text-gray-700 border-gray-300 hover:bg-amber-50' : 'bg-green-700 text-white border-green-700'}`}
            onConfirm={() => send(JSON.stringify({ cmd: 'expiry_enable', expiry: selected, enable: !on, cmdSeq: Date.now() % 100000 }), 'expiry_enable')} />
          {/* qmode off/close are dangerous (stop/cancel), on is recovery */}
          <span className="text-gray-400">qmode</span>
          <button disabled={disabled}
            onClick={() => send(JSON.stringify({ cmd: 'qmode_expiry', expiry: selected, mode: 'on', cmdSeq: Date.now() % 100000 }), 'qmode on')}
            title={`qmode on (hot)${connected ? '' : ' — disconnected'}`}
            className={`${btnBase} bg-white text-gray-700 border-gray-300 ${disabled ? 'opacity-40 cursor-not-allowed' : 'hover:bg-blue-50'}`}>on</button>
          <ConfirmButton key={`off:${selected}`} label="off" confirmLabel="confirm off?"
            disabled={disabled}
            title="qmode off (hot) — pauses quoting for this month"
            className={`${btnBase} bg-white text-gray-700 border-gray-300 ${disabled ? 'opacity-40 cursor-not-allowed' : 'hover:bg-amber-50'}`}
            onConfirm={() => send(JSON.stringify({ cmd: 'qmode_expiry', expiry: selected, mode: 'off', cmdSeq: Date.now() % 100000 }), 'qmode off')} />
          <ConfirmButton key={`close:${selected}`} label="close" confirmLabel="confirm close?"
            disabled={disabled}
            title="qmode close (hot) — reduce-only for this month"
            className={`${btnBase} bg-white text-gray-700 border-gray-300 ${disabled ? 'opacity-40 cursor-not-allowed' : 'hover:bg-amber-50'}`}
            onConfirm={() => send(JSON.stringify({ cmd: 'qmode_expiry', expiry: selected, mode: 'close', cmdSeq: Date.now() % 100000 }), 'qmode close')} />
          {readonly && <span className="text-gray-400">(readonly)</span>}
          {!readonly && !connected && <span className="text-red-500">(disconnected)</span>}
        </>
      )}
      <span className="ml-auto text-gray-400" title="delta-hedge fire threshold (same units as Δ: tonnes for cu)">
        hedge thr {thr > 0 ? `${fmt(thr, 1)}` : 'off'}
      </span>
    </div>
  );
};
