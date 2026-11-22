import React from 'react';
import { useData } from '../context/DataContext';

const fmt = (v: number | null | undefined, d = 0) =>
  v == null || !isFinite(v) ? '-' : v.toFixed(d);

// P1 UI redesign: standalone futures-leg card for the SELECTED expiry —
// market book + our quote target + LIVE futures orders + position.
//
// Audit 11-22: this used to show only `und.ourBid/ourAsk`, which is the
// PRICER's target quote (UnderlyingTradingData::ourMarket), not what is
// actually working at the exchange.  The operator asked for "做市报单情况",
// so the card now also aggregates the futures-leg orders from the
// authoritative snapshot.orders list (same source as the Grid's ordBid /
// ordAsk columns).
export const UnderlierCard: React.FC<{ selected: number | null }> = ({ selected }) => {
  const { snapshot, orders } = useData();
  const e = snapshot?.expiries.find(x => x.expiry === selected) ?? null;
  const u = e?.und ?? null;
  if (!e) return null;

  const mktOk = u && (u.bid ?? 0) > 0 && (u.ask ?? 0) > 0;
  const mid = mktOk ? ((u!.bid! + u!.ask!) / 2) : null;
  const hasQuote = u != null && (u.ourBid != null || u.ourAsk != null);

  const live = u ? Object.values(orders).filter(o => o.code === u.code) : [];
  const ordTxt = (buy: boolean) => live
    .filter(o => o.buy === buy)
    .map(o => `${o.filled > 0 ? `${o.filled}/` : ''}${o.qty}@${o.px ?? '?'}`)
    .join(' ');

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
      <span className="text-gray-500" title="pricer target quote (ourMarket) — NOT necessarily working at the exchange">
        quote
        <b className={hasQuote ? 'text-blue-700' : 'text-gray-400'}>
          {hasQuote
            ? ` ${u!.ourBid != null ? `${fmt(u!.ourBid)}x${fmt(u!.ourBidSz)}` : '-'} / ${u!.ourAsk != null ? `${fmt(u!.ourAsk)}x${fmt(u!.ourAskSz)}` : '-'}`
            : ' -'}
        </b>
      </span>
      <span className="text-gray-500" title="live futures orders from the snapshot (authoritative) — B/S side, qty@px">
        live
        <b className={live.length ? 'text-purple-700' : 'text-gray-400'}>
          {live.length ? ` B ${ordTxt(true) || '-'} · S ${ordTxt(false) || '-'}` : ' -'}
        </b>
      </span>
      <span className={u?.pos ? 'font-bold' : 'text-gray-500'}>pos <b>{fmt(u?.pos, 0)}</b></span>
      <span className="text-gray-500" title="hedge-leg position delta in underlying units (lots x contract size) — the quantity the per-expiry hedge gate uses">
        Δleg <b className={u?.undDelta ? 'text-orange-600' : ''}>{fmt(u?.undDelta, 1)}</b>
      </span>
      <span className="text-[10px] text-gray-400">(month {e.expiry})</span>
    </div>
  );
};
