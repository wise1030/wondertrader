import React from 'react';
import { useData } from '../context/DataContext';
import type { ExpiryView } from '../types';

const fmt = (v: number | null | undefined, d = 2) => (v == null ? '-' : v.toFixed(d));

// batch-5.2: current-vs-target cell — green when within tolerance of the
// target, red when beyond 2x tolerance (or wrong sign for delta).
const TargetCell: React.FC<{ cur: number | null; targ: number | null | undefined; tol?: number }> =
({ cur, targ, tol = 0.15 }) => {
    if (cur == null) return <b>-</b>;
    if (targ == null) return <b>{fmt(cur)}</b>;
    const dev = Math.abs(cur - targ);
    const cls = dev <= Math.max(Math.abs(targ) * tol, 1.0) ? 'text-green-700'
              : dev <= Math.max(Math.abs(targ) * 2 * tol, 2.0) ? 'text-yellow-600'
              : 'text-red-600 font-bold';
    return <b className={cls} title={`cur=${fmt(cur)} targ=${fmt(targ)} dev=${fmt(dev)}`}>{fmt(cur)}</b>;
};

export const ExpirySummaryView: React.FC<{ expiry: ExpiryView | null }> = ({ expiry: e }) => {
    if (!e) return <div className="text-[11px] text-gray-400 px-2 py-1">no expiry selected</div>;

    const Chip: React.FC<{ label: string; on: boolean }> = ({ label, on }) => (
        <span className={`px-1 rounded text-[9px] font-bold ${on ? 'bg-green-100 text-green-800' : 'bg-red-100 text-red-700'}`}>{label}</span>
    );

    return (
        <div className="flex items-center gap-4 px-3 py-1 bg-white border border-gray-200 rounded text-[11px] flex-wrap">
            <span className="font-bold font-mono">{e.expiry}</span>
            <span>DTE <b>{e.daysToExpiry}</b></span>
            <span>fwd <b>{fmt(e.fwd)}</b></span>
            <span>atmVol <b>{fmt(e.atmVol, 4)}</b></span>
            <span>sprd <b>{fmt(e.atmVolSprd, 4)}</b></span>
            {/* batch-5.2: current vs greek target, side by side */}
            <span>Δ <TargetCell cur={e.greeks.delta} targ={e.targDelta} />
                {e.targDelta != null && <span className="text-gray-400">→{fmt(e.targDelta, 0)}</span>}</span>
            <span>ν <TargetCell cur={e.greeks.vega} targ={e.targVega} />
                {e.targVega != null && <span className="text-gray-400">→{fmt(e.targVega, 0)}</span>}</span>
            <span className="text-gray-500">books <b className={e.booksWithBid === 0 ? 'text-red-600' : ''}>{e.booksWithBid}/{e.booksTotal}</b></span>
            <span className="flex gap-1">
                <Chip label="fwd" on={e.fwdReady} />
                <Chip label="fit" on={e.fitReady} />
                <Chip label="und" on={e.underlyingValid} />
            </span>
        </div>
    );
};

export const ExpirySummary: React.FC<{ selected: number | null }> = ({ selected }) => {
    const { snapshot } = useData();
    const e = snapshot?.expiries.find(x => x.expiry === selected) ?? null;
    return <ExpirySummaryView expiry={e} />;
};
