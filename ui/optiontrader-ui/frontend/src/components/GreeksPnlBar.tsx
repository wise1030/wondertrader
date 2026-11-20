import React from 'react';
import { useData } from '../context/DataContext';

const fmt = (v: number | null | undefined, d = 2) => (v == null ? '-' : v.toFixed(d));

export const GreeksPnlBar: React.FC = () => {
    const { snapshot } = useData();
    const g = snapshot?.greeks;
    const p = snapshot?.pnl;

    const pnlColor = (v: number | null | undefined) =>
        v == null ? 'text-gray-400' : v > 0 ? 'text-green-600' : v < 0 ? 'text-red-600' : 'text-gray-700';

    return (
        <div className="flex items-center gap-5 px-3 py-1 bg-white border border-gray-200 rounded text-[11px]">
            <span className="font-semibold text-gray-500">Greeks</span>
            <span>Δ <b>{fmt(g?.delta)}</b></span>
            <span>Γ <b>{fmt(g?.gamma, 4)}</b></span>
            <span>ν <b>{fmt(g?.vega)}</b></span>
            <span>νTW <b>{fmt(g?.vegaTW)}</b></span>
            <span>Θ <b>{fmt(g?.theta)}</b></span>
            <span className="w-px h-4 bg-gray-200" />
            <span className="font-semibold text-gray-500">PnL</span>
            {/* local mark = prev-settle basis + mid valuation; the exchange
                window (CTP) additionally differs by today's fees and uses
                opposite-side liquidation value */}
            <span title="local mark: prev-settle basis, mid valuation, fee-adjusted fills — differs from the CTP position-pnl window by fees and valuation side">
                <span className={pnlColor(p?.cur)}>cur <b>{fmt(p?.cur)}</b></span>
                <span className={pnlColor(p?.realized)}> real <b>{fmt(p?.realized)}</b></span>
                <span className="text-gray-500"> turn <b>{fmt(p?.turnover, 0)}</b></span>
                <span className="ml-1 text-[9px] text-gray-400">(local mark)</span>
            </span>
            {snapshot?.risk.limitsBreached && (
                <span className="ml-auto bg-red-600 text-white px-2 rounded font-bold animate-pulse">LIMITS BREACHED</span>
            )}
        </div>
    );
};
