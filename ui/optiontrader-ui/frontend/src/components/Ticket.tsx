import React, { useState } from 'react';
import { useData } from '../context/DataContext';
import type { LegView } from '../types';

interface Props {
  buysell: 'B' | 'S';
  leg: LegView;
  onClose: () => void;
  onNotify: (msg: string, type: 'info' | 'error') => void;
}

export const Ticket: React.FC<Props> = ({ buysell, leg, onClose, onNotify }) => {
  const { readonly, sendCommand, snapshot } = useData();
  const meta = snapshot?.meta.contracts[leg.code];
  const refPx = buysell === 'B' ? (leg.ask ?? leg.bid ?? 0) : (leg.bid ?? leg.ask ?? 0);
  const [price, setPrice] = useState<string>(refPx ? String(refPx) : '');
  const [qty, setQty] = useState<string>('1');

  const submit = () => {
    if (readonly) { onNotify('readonly mode — order entry is gated', 'info'); return; }
    // P1 fix: reject empty/NaN instead of shipping JSON null downstream.
    const px = parseFloat(price);
    const q = parseInt(qty, 10);
    if (!Number.isFinite(px) || px <= 0) { onNotify('invalid price', 'error'); return; }
    if (!Number.isInteger(q) || q <= 0) { onNotify('invalid qty', 'error'); return; }
    sendCommand(JSON.stringify({
      cmd: 'order', code: leg.code,
      side: buysell, price: px, qty: q,
      cmdSeq: Date.now() % 100000,
    }));
    onNotify(`${buysell} ${q} ${leg.code} @ ${px} sent`, 'info');
    onClose();
  };

  return (
    <div className="fixed inset-0 bg-black/40 flex items-center justify-center z-50" onClick={onClose}>
      <div className="bg-white rounded shadow-xl p-4 w-80 text-xs" onClick={e => e.stopPropagation()}>
        <div className={`font-bold text-sm mb-2 ${buysell === 'B' ? 'text-green-700' : 'text-red-700'}`}>
          {buysell === 'B' ? 'BUY' : 'SELL'} {leg.code}
        </div>
        <div className="grid grid-cols-2 gap-2 mb-3">
          <label className="flex flex-col">
            <span className="text-gray-500">price</span>
            <input className="border rounded px-1 py-0.5" value={price} onChange={e => setPrice(e.target.value)} disabled={readonly} />
          </label>
          <label className="flex flex-col">
            <span className="text-gray-500">qty</span>
            <input className="border rounded px-1 py-0.5" value={qty} onChange={e => setQty(e.target.value)} disabled={readonly} />
          </label>
        </div>
        <div className="text-gray-400 mb-3">
          bid {leg.bid ?? '-'} / ask {leg.ask ?? '-'} · theo {leg.theo ?? '-'} · mult {meta?.mult ?? '?'} tick {meta?.priceTick ?? '?'}
        </div>
        {readonly && (
          <div className="bg-yellow-50 border border-yellow-200 text-yellow-800 rounded px-2 py-1 mb-3">
            READONLY — write commands are gated by the strategy (readonly: true).
          </div>
        )}
        <div className="flex justify-end gap-2">
          <button className="px-3 py-1 rounded bg-gray-200 hover:bg-gray-300" onClick={onClose}>Cancel</button>
          <button
            className={`px-3 py-1 rounded text-white ${readonly ? 'bg-gray-400 cursor-not-allowed' : buysell === 'B' ? 'bg-green-600 hover:bg-green-500' : 'bg-red-600 hover:bg-red-500'}`}
            onClick={submit}
          >
            {buysell === 'B' ? 'Buy' : 'Sell'}
          </button>
        </div>
      </div>
    </div>
  );
};
