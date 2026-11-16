import React, { useEffect, useMemo, useState, useCallback, memo } from 'react';
import { useData } from '../context/DataContext';
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
}

const fmtN = (v: number | null | undefined, d = 2) => (v == null ? '-' : v.toFixed(d));

// Qt qmode colors (from the original UI): on=green-700, auto=blue-300, close=green-300, off=red-200
const qmodeClass = (q: number | null): string => {
  if (q === 1) return 'bg-green-700 text-white';
  if (q === 0) return 'bg-blue-300 text-gray-900';
  if (q === 2) return 'bg-green-300 text-gray-900';
  if (q === -1) return 'bg-red-200 text-gray-900';
  return '';
};

const posClass = (pos: number | null | undefined): string => {
  if (!pos) return 'text-gray-700';
  return pos > 0 ? 'text-green-600 font-bold' : 'text-red-600 font-bold';
};

const MemoizedGridRow = memo(({
  row, idx, vshiftVisible, onCellClick, onOpenTicket, isCurrent, onRowClick, readonly
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
          <td className={cell}>-</td><td className={cell}>-</td>
        </>
      );
    }
    const ticket = (bs: 'B' | 'S') => onOpenTicket(bs, leg);
    const qCls = qmodeClass(leg.qmode);
    return (
      <>
        <td className={`${cell}`} onClick={() => ticket('S')}>{leg.bidSz ?? '-'}</td>
        <td className={`${cell}`} onClick={() => ticket('S')}>{leg.bid ?? '-'}</td>
        <td className={`${cell} bg-gray-50`}>{leg.last ?? '-'}</td>
        <td className={`${cell} font-medium`} onClick={() => onCellClick(row, side, 'theo')}>{fmtN(leg.theo)}</td>
        <td className={`${cell} bg-gray-50`}>{fmtN(leg.iv, 4)}</td>
        <td className={`${cell}`} onClick={() => ticket('B')}>{leg.ask ?? '-'}</td>
        <td className={`${cell}`} onClick={() => ticket('B')}>{leg.askSz ?? '-'}</td>
        <td className={`${cell} text-blue-600`}>{fmtN(leg.delta, 4)}</td>
        {vshiftVisible && <td className={`${cell} text-gray-500`}>{fmtN(leg.vshift)}</td>}
        <td
          className={`${cell} ${qCls} ${posClass(leg.pos)}`}
          title={readonly ? `qmode=${leg.qmode ?? '?'}` : 'double-click toggles qmode'}
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
}

export const OptionGridView: React.FC<Props> = ({ selectedExpiry, onExpirySelectByIndex, onNotify }) => {
  const { snapshot, readonly } = useData();
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

  const openTicket = useCallback((buysell: 'B' | 'S', leg: LegView) => {
    setTicketData({ buysell, leg });
    setIsTicketOpen(true);
  }, []);

  const handleCellClick = useCallback((_row: GridRow, _side: 'C' | 'P', type: string) => {
    if (type === 'pos_dbl') {
      // P2: per-code qmode is not in the CommandBus (QMODE is grid-wide);
      // the double-click only explains where the control lives.
      onNotify('qmode is grid-wide in P2 — use the command bar buttons', 'info');
      return;
    }
  }, [onNotify]);

  // Qt keyboard shortcuts (navigation/toggles live; trading shortcuts gated by readonly)
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (e.ctrlKey && e.key === 'v') { e.preventDefault(); setVshiftVisible(p => !p); return; }
      if (e.key.startsWith('F') && e.key.length <= 3) {
        const n = parseInt(e.key.substring(1));
        if (n >= 1 && n <= 8) { e.preventDefault(); onExpirySelectByIndex(n - 1); return; }
      }
      if (e.key === 'Escape') { setSelectedRowIdx(null); return; }
      if (selectedRowIdx === null || rows.length === 0) return;
      if (e.key === 'ArrowUp') { e.preventDefault(); setSelectedRowIdx(p => (p !== null && p > 0 ? p - 1 : p)); return; }
      if (e.key === 'ArrowDown') { e.preventDefault(); setSelectedRowIdx(p => (p !== null && p < rows.length - 1 ? p + 1 : p)); return; }
      if (e.ctrlKey && ['=', '+', '-', '0', 'Backspace'].includes(e.key)) {
        e.preventDefault();
        onNotify(readonly ? 'readonly mode — qmode shortcuts are P2' : 'qmode shortcut', 'info');
      }
      if (!e.ctrlKey && (e.key === '+' || e.key === '=')) {
        e.preventDefault();
        const leg = rows[selectedRowIdx]?.call;
        if (leg) openTicket('B', leg);
      } else if (!e.ctrlKey && e.key === '-') {
        e.preventDefault();
        const leg = rows[selectedRowIdx]?.call;
        if (leg) openTicket('S', leg);
      }
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [selectedRowIdx, rows, readonly, onExpirySelectByIndex, openTicket, onNotify]);

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
              <th className="border border-gray-300 px-1 py-0.5" colSpan={7}>CALL</th>
              <th className="border border-gray-300 px-1 py-0.5 bg-purple-200 text-purple-800">strike</th>
              <th className="border border-gray-300 px-1 py-0.5" colSpan={7}>PUT</th>
            </tr>
            <tr className="bg-gray-100 text-gray-600">
              {['bidSz', 'bid', 'last', 'theo', 'iv', 'ask', 'askSz', 'delta', ...(vshiftVisible ? ['vshift'] : []), 'pos', 'pnl'].map(h => (
                <React.Fragment key={h}>
                  <th className="border border-gray-300 px-1 py-0.5">{h}</th>
                </React.Fragment>
              ))}
              <th />
              {['bidSz', 'bid', 'last', 'theo', 'iv', 'ask', 'askSz', 'delta', ...(vshiftVisible ? ['vshift'] : []), 'pos', 'pnl'].map(h => (
                <React.Fragment key={h}>
                  <th className="border border-gray-300 px-1 py-0.5">{h}</th>
                </React.Fragment>
              ))}
            </tr>
          </thead>
          <tbody>
            {rows.map((row, idx) => (
              <MemoizedGridRow
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
                <td colSpan={26} className="text-center py-12 text-gray-600 bg-gray-50">
                  <div className="text-3xl mb-2">📊</div>
                  <div>No strikes for this expiry (check fit-gate: booksWithBid / fwdReady)</div>
                </td>
              </tr>
            )}
          </tbody>
        </table>
      </div>

      {isTicketOpen && ticketData && (
        <Ticket
          buysell={ticketData.buysell}
          leg={ticketData.leg}
          onClose={() => setIsTicketOpen(false)}
          onNotify={onNotify}
        />
      )}
    </div>
  );
};
