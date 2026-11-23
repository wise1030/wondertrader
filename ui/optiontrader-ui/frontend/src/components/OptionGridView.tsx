import React, { useEffect, useMemo, useState, useCallback, memo } from 'react';
import { useData } from '../context/DataContext';
import { useNowMs } from '../hooks/useNow';
import type { LegView, StrikeView } from '../types';
import { Ticket } from './Ticket';

interface GridRow {
  strike: number;
  call?: LegView;
  put?: LegView;
}

interface GridRowProps {
  row: GridRow;
  idx: number;
  vshiftVisible: boolean;
  onCellClick: (row: GridRow, side: 'C' | 'P', type: string) => void;
  onOpenTicket: (buysell: 'B' | 'S', leg: LegView) => void;
  isCurrent: boolean;
  onRowClick: (idx: number) => void;
  readonly: boolean;
  // P1 (2026-11-19): live orders keyed by stdCode, aggregated per side and
  // rendered as TWO separate columns (ordBid = our resting bids, ordAsk =
  // our resting asks) so both sides of the market-making quote are visible
  // at a glance.
  ordersByCode: Record<string, { B: string; S: string }>;
  // P1 batch-2: latest-fill flash (code -> ms) + ticking clock for the fade
  flash: Record<string, number>;
  nowMs: number;
}

const fmtN = (v: number | null | undefined, d = 2) => (v == null ? '-' : v.toFixed(d));

// Qt qmode colors (from the original UI): on=green-700, auto=blue-300, close=green-300, off=red-200
const qmodeClass = (q: number | null): string => {
  if (q === 1) return 'bg-green-700 text-white';
  if (q === 0) return 'bg-blue-300 text-gray-900';
  if (q === 2) return 'bg-green-300 text-gray-900';
  if (q === -1) return 'bg-red-200 text-gray-900';
  if (q === 3) return 'bg-yellow-200 text-gray-900';   // QM_FLAT (optioncoretypes.h)
  return '';
};

const qmodeName = (q: number | null): string => {
  if (q === 1) return 'on';
  if (q === 0) return 'auto';
  if (q === -1) return 'off';
  if (q === 2) return 'close';
  if (q === 3) return 'flat';
  return q == null ? '?' : String(q);
};

// P1 batch-2: queue-position background, ported from quantbox OptionGridModel
// quoteBackgroundFormatter: 0 = at best (lightest), >0 = ticks behind (light
// tint), <0 = crossing (solid), 9999 = no quote (none).
const qpBg = (qp: number | undefined, side: 'B' | 'S'): string => {
    if (qp == null || qp >= 9999) return '';
    if (qp < 0) return side === 'B' ? 'bg-green-500 text-white' : 'bg-red-500 text-white';
    if (qp === 0) return side === 'B' ? 'bg-green-100' : 'bg-red-100';
    return side === 'B' ? 'bg-green-200' : 'bg-red-200';
};

// sqrt-t fade over 3s (quantbox FadeTimer semantics)
const flashOpacity = (flashMs: number | undefined, nowMs: number): number | null => {
    if (!flashMs) return null;
    const e = nowMs - flashMs;
    if (e < 0 || e > 3000) return null;
    return 1.0 - Math.sqrt(e / 3000);
};

const posClass = (pos: number | null | undefined): string => {
  if (!pos) return 'text-gray-700';
  return pos > 0 ? 'text-green-600 font-bold' : 'text-red-600 font-bold';
};

const MemoizedGridRow = memo(({
  row, idx, vshiftVisible, onCellClick, onOpenTicket, isCurrent, onRowClick, readonly, ordersByCode,
  flash, nowMs
}: GridRowProps) => {
  const cell = 'border border-gray-300 px-1 py-0.5 text-center text-xs';

  const legCells = (side: 'C' | 'P', leg?: LegView) => {
    if (!leg) {
      return (
        <>
          <td className={cell}>-</td><td className={cell}>-</td><td className={cell}>-</td>
          <td className={cell}>-</td><td className={cell}>-</td><td className={cell}>-</td>
          <td className={cell}>-</td><td className={cell}>-</td>
          {vshiftVisible && <td className={cell}>-</td>}
          <td className={cell}>-</td><td className={cell}>-</td><td className={cell}>-</td><td className={cell}>-</td>
        </>
      );
    }
    const ticket = (bs: 'B' | 'S') => onOpenTicket(bs, leg);
    const qCls = qmodeClass(leg.qmode);
    const ourOrd = ordersByCode[leg.code];
    return (
      <>
        {(() => (
          <td className={`${cell} text-[9px] text-green-700 whitespace-pre ${qpBg(leg.bqp, 'B')}`}
              title={`our bid ${leg.obid ?? '-'} · queue pos ${leg.bqp === 9999 ? 'none' : leg.bqp}${leg.bqp === 0 ? ' (best)' : leg.bqp != null && leg.bqp < 0 ? ' (crossing!)' : ''}`}>
            {ourOrd?.B || (leg.obid ? `${leg.obid}` : '-')}
          </td>
        ))()}
        {(() => {
          const fo = flashOpacity(flash[leg.code], nowMs);
          return (
            <td className={`${cell}`} onClick={() => ticket('S')}
                style={fo != null ? { opacity: 0.3 + 0.7 * fo, backgroundColor: `rgba(59,130,246,${0.55 * fo})` } : undefined}>
              {leg.bidSz ?? '-'}
              {/* Audit 11-22: five-level bid depth was fetched but never
                  rendered (only the ask side was) — symmetric now. */}
              {leg.bd && leg.bd.length > 0 && (
                <span className="text-[8px] text-gray-400"
                  title={leg.bd.map(l => `${l[0]}x${l[1]}`).join(' | ')}>
                  +{leg.bd.reduce((a, l) => a + (l[1] || 0), 0)}
                </span>
              )}
            </td>
          );
        })()}
        <td className={`${cell}`} onClick={() => ticket('S')}>{leg.bid ?? '-'}</td>
        <td className={`${cell} bg-gray-50`}>{leg.last ?? '-'}</td>
        <td className={`${cell} font-medium`} onClick={() => onCellClick(row, side, 'theo')}>{fmtN(leg.theo)}</td>
        <td className={`${cell} bg-gray-50`}>{fmtN(leg.iv, 4)}</td>
        <td className={`${cell}`} onClick={() => ticket('B')}>{leg.ask ?? '-'}</td>
        <td className={`${cell}`} onClick={() => ticket('B')}>
          {leg.askSz ?? '-'}
          {leg.ad && leg.ad.length > 0 && (
            <span className="text-[8px] text-gray-400"
              title={leg.ad.map(l => `${l[0]}x${l[1]}`).join(' | ')}>
              +{leg.ad.reduce((a, l) => a + (l[1] || 0), 0)}
            </span>
          )}
        </td>
        {(() => (
          <td className={`${cell} text-[9px] text-red-700 whitespace-pre ${qpBg(leg.aqp, 'S')}`}
              title={`our ask ${leg.oask ?? '-'} · queue pos ${leg.aqp === 9999 ? 'none' : leg.aqp}${leg.aqp === 0 ? ' (best)' : leg.aqp != null && leg.aqp < 0 ? ' (crossing!)' : ''}`}>
            {ourOrd?.S || (leg.oask ? `${leg.oask}` : '-')}
          </td>
        ))()}
        <td className={`${cell} text-blue-600`}>{fmtN(leg.delta, 4)}</td>
        {vshiftVisible && <td className={`${cell} text-gray-500`}>{fmtN(leg.vshift)}</td>}
        <td
          className={`${cell} ${qCls} ${posClass(leg.pos)}`}
          title={`qmode ${qmodeName(leg.qmode)}${readonly ? ' (readonly)' : ' — double-click toggles on/off'}`}
          onDoubleClick={() => onCellClick(row, side, 'pos_dbl')}
        >
          {leg.pos || 0}
        </td>
        <td className={`${cell} ${posClass(leg.pnl)}`}>{fmtN(leg.pnl)}</td>
      </>
    );
  };

  return (
    <tr
      className={`${isCurrent ? 'bg-yellow-100' : (idx % 2 === 0 ? 'bg-white' : 'bg-gray-50')} hover:bg-yellow-50 cursor-pointer`}
      onClick={() => onRowClick(idx)}
    >
      {legCells('C', row.call)}
      <td className={`${cell} bg-gray-200 font-mono`}>{row.strike}</td>
      {legCells('P', row.put)}
    </tr>
  );
});
MemoizedGridRow.displayName = 'MemoizedGridRow';

interface Props {
  selectedExpiry: number | null;
  onExpirySelectByIndex: (i: number) => void;
  onNotify: (msg: string, type: 'info' | 'error') => void;
  /// Audit 11-22: the tab body stays mounted while hidden, so the global
  /// keydown handler must only act when the Grid is the visible tab.
  active?: boolean;
}

export const OptionGridView: React.FC<Props> = ({ selectedExpiry, onExpirySelectByIndex, onNotify, active = true }) => {
  const { snapshot, readonly, sendCommand, orders, flash, connectionStatus } = useData();
  const nowMs = useNowMs(250);   // P1 batch-2: drives the sqrt-t fill fade
  // P1: aggregate live orders per contract: "B:qty@px" / "S:qty@px"
  const ordersByCode = React.useMemo(() => {
    const m: Record<string, { B: string; S: string }> = {};
    for (const o of Object.values(orders)) {
      const e = (m[o.code] ??= { B: '', S: '' });
      const side = o.buy ? 'B' : 'S';
      const txt = `${o.filled > 0 ? `${o.filled}/` : ''}${o.qty}@${o.px ?? '?'}`;
      e[side] = e[side] ? e[side] + ' ' + txt : txt;
    }
    return m;
  }, [orders]);
  const [vshiftVisible, setVshiftVisible] = useState(false);
  const [selectedRowIdx, setSelectedRowIdx] = useState<number | null>(null);
  const [isTicketOpen, setIsTicketOpen] = useState(false);
  const [ticketData, setTicketData] = useState<{ buysell: 'B' | 'S'; leg: LegView } | null>(null);

  const exp = useMemo(
    () => snapshot?.expiries.find(e => e.expiry === selectedExpiry) ?? null,
    [snapshot, selectedExpiry]
  );

  const rows = useMemo<GridRow[]>(() => {
    const strikes: StrikeView[] = exp?.strikes ?? [];
    return [...strikes]
      .sort((a, b) => a.k - b.k)
      .map(s => ({ strike: s.k, call: s.C.code ? s.C : undefined, put: s.P.code ? s.P : undefined }));
  }, [exp]);

  // P1 fix: remember the last interacted leg side so the +/- shortcuts can
  // open a PUT ticket too (previously both keys always targeted the call).
  const [lastSide, setLastSide] = useState<'C' | 'P'>('C');
  const openTicket = useCallback((buysell: 'B' | 'S', leg: LegView) => {
    setLastSide(leg.code.includes('.P.') ? 'P' : 'C');
    setTicketData({ buysell, leg });
    setIsTicketOpen(true);
  }, []);

  const handleCellClick = useCallback((row: GridRow, side: 'C' | 'P', type: string) => {
    if (type === 'pos_dbl') {
      const leg = side === 'C' ? row.call : row.put;
      if (!leg) return;
      if (readonly) {
        onNotify('readonly mode — qmode toggle is gated', 'info');
        return;
      }
      // Audit 11-22 (round 2): same no-pretend contract as the rest — a
      // disconnected double-click used to toast "qmode -> off" while nothing
      // was ever sent.
      if (connectionStatus !== 'connected') {
        onNotify('not connected — qmode NOT sent', 'error');
        return;
      }
      // P3: single-code qmode toggle (off <-> on), matching the original UI's
      // double-click habit; grid-wide stays on the command bar.
      const next = leg.qmode === -1 ? 'on' : 'off';
      const ok = sendCommand(JSON.stringify({
        cmd: 'qmode_code', code: leg.code, mode: next,
        cmdSeq: Date.now() % 100000,
      }));
      if (!ok) { onNotify('not connected — qmode NOT sent', 'error'); return; }
      onNotify(`${leg.code} qmode -> ${next}`, 'info');
    }
  }, [readonly, sendCommand, onNotify, connectionStatus]);

  // Qt keyboard shortcuts (navigation/toggles live; trading shortcuts gated by readonly)
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (!active) return;   // hidden tab must not steal F1-F8 / +/- / arrows
      // Escape closes the ticket even while typing in it.
      if (e.key === 'Escape') {
        if (isTicketOpen) { setIsTicketOpen(false); return; }
      }
      // Focus hygiene: never hijack keys while the user is typing — Ctrl+V
      // must paste, digits/+/- must type (audit batch B, M8).
      const t = e.target as HTMLElement | null;
      if (t && (t.tagName === 'INPUT' || t.tagName === 'TEXTAREA' || t.tagName === 'SELECT' || t.isContentEditable)) return;
      if (e.ctrlKey && e.key === 'v') { e.preventDefault(); setVshiftVisible(p => !p); return; }
      if (e.key.startsWith('F') && e.key.length <= 3) {
        const n = parseInt(e.key.substring(1));
        if (n >= 1 && n <= 8) { e.preventDefault(); onExpirySelectByIndex(n - 1); return; }
      }
      if (e.key === 'Escape') { setSelectedRowIdx(null); return; }
      if (selectedRowIdx === null || rows.length === 0) return;
      if (e.key === 'ArrowUp') { e.preventDefault(); setSelectedRowIdx(p => (p !== null && p > 0 ? p - 1 : p)); return; }
      if (e.key === 'ArrowDown') { e.preventDefault(); setSelectedRowIdx(p => (p !== null && p < rows.length - 1 ? p + 1 : p)); return; }
      if (!e.ctrlKey && (e.key === '+' || e.key === '=')) {
        e.preventDefault();
        const leg = lastSide === 'P' ? rows[selectedRowIdx]?.put : rows[selectedRowIdx]?.call;
        if (leg) openTicket('B', leg);
      } else if (!e.ctrlKey && e.key === '-') {
        e.preventDefault();
        const leg = lastSide === 'P' ? rows[selectedRowIdx]?.put : rows[selectedRowIdx]?.call;
        if (leg) openTicket('S', leg);
      }
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [selectedRowIdx, rows, onExpirySelectByIndex, openTicket, isTicketOpen, lastSide, active]);

  return (
    <div className="flex flex-col h-full bg-gray-50 text-[10px] overflow-hidden">
      <div className="flex items-center p-2 bg-white border-b border-gray-300 gap-4">
        <label className="flex items-center">
          <input type="checkbox" checked={vshiftVisible} onChange={e => setVshiftVisible(e.target.checked)} className="mr-1" />
          VShift (Ctrl+V)
        </label>
        <span className="text-gray-400">{exp ? `${exp.expiry} · ${rows.length} strikes · books ${exp.booksWithBid}/${exp.booksTotal}` : 'no expiry selected'}</span>
        {readonly && <span className="ml-auto bg-gray-200 text-gray-600 px-2 rounded text-[10px] font-bold">READONLY</span>}
      </div>

      <div className="flex-1 overflow-auto bg-white">
        <table className="w-full border-collapse text-[10px]">
          <thead className="sticky top-0 z-10 bg-white">
            <tr className="bg-gray-200 text-gray-700">
              <th className="border border-gray-300 px-1 py-0.5" colSpan={vshiftVisible ? 13 : 12}>CALL</th>
              <th className="border border-gray-300 px-1 py-0.5 bg-purple-200 text-purple-800">strike</th>
              <th className="border border-gray-300 px-1 py-0.5" colSpan={vshiftVisible ? 13 : 12}>PUT</th>
            </tr>
            <tr className="bg-gray-100 text-gray-600">
              {['ordBid', 'bidSz', 'bid', 'last', 'theo', 'iv', 'ask', 'askSz', 'ordAsk', 'delta', ...(vshiftVisible ? ['vshift'] : []), 'pos', 'pnl'].map(h => (
                <React.Fragment key={h}>
                  <th className="border border-gray-300 px-1 py-0.5">{h}</th>
                </React.Fragment>
              ))}
              <th />
              {['ordBid', 'bidSz', 'bid', 'last', 'theo', 'iv', 'ask', 'askSz', 'ordAsk', 'delta', ...(vshiftVisible ? ['vshift'] : []), 'pos', 'pnl'].map(h => (
                <React.Fragment key={h}>
                  <th className="border border-gray-300 px-1 py-0.5">{h}</th>
                </React.Fragment>
              ))}
            </tr>
          </thead>
          <tbody>
            {rows.map((row, idx) => (
              <MemoizedGridRow
                ordersByCode={ordersByCode}
                flash={flash}
                nowMs={nowMs}
                key={row.strike}
                row={row}
                idx={idx}
                vshiftVisible={vshiftVisible}
                onCellClick={handleCellClick}
                onOpenTicket={openTicket}
                isCurrent={selectedRowIdx === idx}
                onRowClick={setSelectedRowIdx}
                readonly={readonly}
              />
            ))}
            {rows.length === 0 && (
              <tr>
                <td colSpan={vshiftVisible ? 27 : 25} className="text-center py-12 text-gray-600 bg-gray-50">
                  <div className="text-3xl mb-2">📊</div>
                  <div>No strikes for this expiry (check fit-gate: booksWithBid / fwdReady)</div>
                </td>
              </tr>
            )}
          </tbody>
        </table>
      </div>

      {isTicketOpen && ticketData && (
        // P1 fix: key by leg+side so switching targets while the ticket is
        // open REMOUNTS it — without the key the stale price/qty of the
        // previous contract survived the switch.
        <Ticket
          key={`${ticketData.leg.code}:${ticketData.buysell}`}
          buysell={ticketData.buysell}
          leg={ticketData.leg}
          onClose={() => setIsTicketOpen(false)}
          onNotify={onNotify}
        />
      )}
    </div>
  );
};
