import React from 'react';
import { useData } from '../context/DataContext';

interface Props {
    selected: number | null;
    onSelect: (expiry: number) => void;
}

export const ExpirySelector: React.FC<Props> = ({ selected, onSelect }) => {
    const { snapshot } = useData();
    const expiries = snapshot?.expiries ?? [];

    return (
        <div className="flex items-center gap-1.5 flex-wrap px-2 py-1 bg-white border border-gray-200 rounded">
            <span className="text-[10px] text-gray-400 mr-1">expiry (F1-F8)</span>
            {expiries.map((e, i) => {
                const active = e.expiry === selected;
                return (
                    <button
                        key={e.expiry}
                        onClick={() => onSelect(e.expiry)}
                        className={`px-2 py-0.5 rounded text-[11px] font-mono border flex items-center gap-1
                            ${active ? 'bg-blue-600 text-white border-blue-600' : 'bg-gray-50 hover:bg-gray-100 border-gray-300'}`}
                        title={`fwdReady=${e.fwdReady} fitReady=${e.fitReady} books=${e.booksWithBid}/${e.booksTotal}`}
                    >
                        <span>{i < 8 ? `F${i + 1} ` : ''}{e.expiry}</span>
                        <span className={`w-1.5 h-1.5 rounded-full ${e.fitReady ? 'bg-green-400' : e.fwdReady ? 'bg-yellow-400' : 'bg-red-400'}`} />
                    </button>
                );
            })}
            {expiries.length === 0 && <span className="text-[11px] text-gray-400">no expiries in snapshot</span>}
        </div>
    );
};
