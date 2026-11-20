import React, { useMemo } from 'react';
import { useData } from '../context/DataContext';

interface Props {
    selected: number | null;
    onSelect: (expiry: number) => void;
}

export const ExpirySelector: React.FC<Props> = ({ selected, onSelect }) => {
    const { snapshot } = useData();
    // batch-5.3: enable+ready series first (quoting on AND fitReady), then
    // enabled-but-not-ready, then disabled; original expiry order within each
    // tier so the F1..Fn hotkeys still map to the visible order.
    const expiries = useMemo(() => {
        const list = [...(snapshot?.expiries ?? [])];
        const tier = (e: (typeof list)[number]) =>
            e.quoting !== false && e.fitReady ? 0
          : e.quoting !== false ? 1
          : 2;
        return list.sort((a, b) => tier(a) - tier(b) || a.expiry - b.expiry);
    }, [snapshot?.expiries]);

    return (
        <div className="flex items-center gap-1.5 flex-wrap px-2 py-1 bg-white border border-gray-200 rounded">
            <span className="text-[10px] text-gray-400 mr-1">expiry (F1-F8)</span>
            {/* batch-5.3: compact dropdown jump/filter — the sorted button row
                stays the primary control; the select is the keyboard-free path
                when many series are configured. */}
            <select
                className="border border-gray-300 rounded text-[10px] px-1 py-0.5 bg-gray-50"
                value={selected ?? ''}
                onChange={ev => ev.target.value && onSelect(Number(ev.target.value))}
                title="jump to expiry"
            >
                <option value="">jump…</option>
                {expiries.map(e => (
                    <option key={e.expiry} value={e.expiry}>
                        {e.expiry}{e.quoting === false ? ' (off)' : e.fitReady ? ' ✓' : ''}
                    </option>
                ))}
            </select>
            {expiries.map((e, i) => {
                const active = e.expiry === selected;
                const off = e.quoting === false;   // P1: grey out disabled series
                return (
                    <button
                        key={e.expiry}
                        onClick={() => onSelect(e.expiry)}
                        className={`px-2 py-0.5 rounded text-[11px] font-mono border flex items-center gap-1
                            ${off ? 'opacity-40 bg-gray-100 border-gray-200 text-gray-400 cursor-not-allowed'
                                  : active ? 'bg-blue-600 text-white border-blue-600' : 'bg-gray-50 hover:bg-gray-100 border-gray-300'}`}
                        title={`quoting=${e.quoting !== false} fwdReady=${e.fwdReady} fitReady=${e.fitReady} books=${e.booksWithBid}/${e.booksTotal}`}
                    >
                        <span>{i < 8 ? `F${i + 1} ` : ''}{e.expiry}</span>
                        <span className={`w-1.5 h-1.5 rounded-full ${off ? 'bg-gray-300' : e.fitReady ? 'bg-green-400' : e.fwdReady ? 'bg-yellow-400' : 'bg-red-400'}`} />
                    </button>
                );
            })}
            {expiries.length === 0 && <span className="text-[11px] text-gray-400">no expiries in snapshot</span>}
        </div>
    );
};
