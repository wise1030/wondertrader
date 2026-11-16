import React from 'react';
import { useData } from '../context/DataContext';

const fmt = (v: number | null | undefined, d = 1) => (v == null ? '-' : v.toFixed(d));

export const UnderlierView: React.FC<{ selected: number | null }> = ({ selected }) => {
    const { snapshot } = useData();
    const e = snapshot?.expiries.find(x => x.expiry === selected);
    const und = e?.und;

    return (
        <div className="flex items-center gap-4 px-3 py-1 bg-white border border-gray-200 rounded text-[11px]">
            <span className="font-semibold text-gray-500">hedge</span>
            <span className="font-mono">{und?.code ?? '-'}</span>
            <span>bid <b>{fmt(und?.bid)}</b></span>
            <span>ask <b>{fmt(und?.ask)}</b></span>
            <span className={und && und.pos !== 0 ? 'font-bold' : ''}>pos <b>{fmt(und?.pos, 0)}</b></span>
        </div>
    );
};
