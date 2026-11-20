import React from 'react';
import { useData } from '../context/DataContext';

const fmt = (v: number | null | undefined, d = 0) =>
  v == null || !isFinite(v) ? '-' : v.toFixed(d);

// P1 UI redesign: standalone futures-leg card for the SELECTED expiry —
// market book + our market-making quotes + position.  Split out of the
// BOARD columns per the operator's layout feedback (futures info deserves
// its own surface, mirroring what the option legs show in the Grid).
export const UnderlierCard: React.FC<{ selected: number | null }> = ({ selected }) => {
  const { snapshot } = useData();
  const e = snapshot?.expiries.find(x => x.expiry === selected) ?? null;
  const u = e?.und ?? null;
  if (!e) return null;

  const mktOk = u && (u.bid ?? 0) > 0 && (u.ask ?? 0) > 0;
  const ourOk = u && u.ourBid != null;
  const mid = mktOk ? ((u!.bid! + u!.ask!) / 2) : null;

  return (
    <div className="bg-white border border-gray-200 rounded px-3 py-1 text-[11px] flex items-center gap-4 flex-wrap">
      <span className="font-semibold text-gray-500">futures leg</span>
      <span className="font-mono font-bold">{u?.code ?? '-'}</span>
      <span className="text-gray-500">mkt
        <b className={mktOk ? '' : 'text-gray-400'}>
          {mktOk ? ` ${fmt(u!.bid)}x${fmt(u!.bidSz)} / ${fmt(u!.ask)}x${fmt(u!.askSz)}` : ' -'}
        </b>
      </span>
      <span className="text-gray-500">mid <b>{mid != null ? fmt(mid) : '-'}</b></span>
      <span className="text-gray-500">our
        {ourOk
          ? <b className="text-blue-700"> {fmt(u!.ourBid)}x{fmt(u!.ourBidSz)} / {fmt(u!.ourAsk)}x{fmt(u!.ourAskSz)}</b>
          : ' -'}
      </span>
      <span className={u?.pos ? 'font-bold' : 'text-gray-500'}>pos <b>{fmt(u?.pos, 0)}</b></span>
      <span className="text-[10px] text-gray-400">(month {e.expiry})</span>
    </div>
  );
};
