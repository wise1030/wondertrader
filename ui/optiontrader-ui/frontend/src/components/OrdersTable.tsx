import React from 'react';
import { useData } from '../context/DataContext';
import { tagLabel, tagClass } from './TradesTab';

export const OrdersTable: React.FC = () => {
  const { orders, readonly, sendCommand } = useData();
  const list = Object.values(orders).sort((a, b) => b.id - a.id);

  const cancel = (id: number) => {
    // P1 UI redesign: notify instead of silent return (consistent style)
    if (readonly) { console.warn('[OrdersTable] cancel skipped (readonly)'); return; }
    sendCommand(JSON.stringify({ cmd: 'cancel_order', id, cmdSeq: Date.now() % 100000 }));
  };

  const cancelAll = () => {
    if (readonly) return;
    sendCommand(JSON.stringify({ cmd: 'cancel_all', cmdSeq: Date.now() % 100000 }));
  };

  return (
    <div>
      <div className="flex items-center justify-between mb-1">
        <span className="text-xs font-bold text-gray-600">Live orders (snapshot is authoritative)</span>
        {!readonly && (
          <button
            className="px-2 py-0.5 bg-red-600 hover:bg-red-500 text-white rounded text-[10px] font-bold"
            onClick={cancelAll}
          >
            Cancel all
          </button>
        )}
      </div>
      <table className="w-full border-collapse text-[11px]">
      <thead>
        <tr className="bg-gray-200 text-gray-700">
          <th className="border border-gray-300 px-1 py-0.5 text-left">id</th>
          <th className="border border-gray-300 px-1 py-0.5 text-left">code</th>
          <th className="border border-gray-300 px-1 py-0.5">src</th>
          <th className="border border-gray-300 px-1 py-0.5">dir</th>
          <th className="border border-gray-300 px-1 py-0.5">px</th>
          <th className="border border-gray-300 px-1 py-0.5">qty</th>
          <th className="border border-gray-300 px-1 py-0.5">filled</th>
          <th className="border border-gray-300 px-1 py-0.5">rem</th>
          <th className="border border-gray-300 px-1 py-0.5">{readonly ? '' : 'cxl'}</th>
        </tr>
      </thead>
      <tbody>
        {list.map(o => (
          <tr
            key={o.id}
            className="hover:bg-yellow-50"
            title={readonly ? undefined : 'double-click to cancel'}
            onDoubleClick={() => cancel(o.id)}
          >
            <td className="border border-gray-200 px-1 font-mono">{o.id}</td>
            <td className="border border-gray-200 px-1 font-mono">{o.code}</td>
            <td className="border border-gray-200 px-1 text-center"><span className={`px-0.5 border rounded text-[9px] ${tagClass(o.tag)}`}>{tagLabel(o.tag)}</span></td>
            <td className={`border border-gray-200 px-1 text-center font-bold ${o.buy ? 'text-green-700' : 'text-red-700'}`}>{o.buy ? 'B' : 'S'}</td>
            <td className="border border-gray-200 px-1 text-right">{o.px ?? '-'}</td>
            <td className="border border-gray-200 px-1 text-right">{o.qty}</td>
            <td className="border border-gray-200 px-1 text-right">{o.filled}</td>
            <td className="border border-gray-200 px-1 text-right">{o.qty - o.filled}</td>
            <td className="border border-gray-200 px-1 text-center">
              {!readonly && (
                <button
                  className="px-1.5 bg-red-100 hover:bg-red-200 text-red-700 rounded text-[10px]"
                  onClick={e => { e.stopPropagation(); cancel(o.id); }}
                >
                  ✕
                </button>
              )}
            </td>
          </tr>
        ))}
        {list.length === 0 && (
          <tr><td colSpan={8} className="text-center py-6 text-gray-400">no live orders</td></tr>
        )}
      </tbody>
    </table>
    </div>
  );
};
