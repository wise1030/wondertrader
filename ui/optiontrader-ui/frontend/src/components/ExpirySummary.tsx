import React from 'react';
import { useData } from '../context/DataContext';
import type { ExpiryView } from '../types';

const fmt = (v: number | null | undefined, d = 2) => (v == null ? '-' : v.toFixed(d));

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
            <span>Δ <b>{fmt(e.greeks.delta)}</b></span>
            <span>ν <b>{fmt(e.greeks.vega)}</b></span>
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
