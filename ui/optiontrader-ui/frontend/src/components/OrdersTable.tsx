import React from 'react';
import { useData } from '../context/DataContext';

export const OrdersTable: React.FC = () => {
  const { orders } = useData();
  const list = Object.values(orders).sort((a, b) => b.id - a.id);

  return (
    <table className="w-full border-collapse text-[11px]">
      <thead>
        <tr className="bg-gray-200 text-gray-700">
          <th className="border border-gray-300 px-1 py-0.5 text-left">id</th>
          <th className="border border-gray-300 px-1 py-0.5 text-left">code</th>
          <th className="border border-gray-300 px-1 py-0.5">dir</th>
          <th className="border border-gray-300 px-1 py-0.5">px</th>
          <th className="border border-gray-300 px-1 py-0.5">qty</th>
          <th className="border border-gray-300 px-1 py-0.5">filled</th>
          <th className="border border-gray-300 px-1 py-0.5">rem</th>
        </tr>
      </thead>
      <tbody>
        {list.map(o => (
          <tr key={o.id} className="hover:bg-yellow-50">
            <td className="border border-gray-200 px-1 font-mono">{o.id}</td>
            <td className="border border-gray-200 px-1 font-mono">{o.code}</td>
            <td className={`border border-gray-200 px-1 text-center font-bold ${o.buy ? 'text-green-700' : 'text-red-700'}`}>{o.buy ? 'B' : 'S'}</td>
            <td className="border border-gray-200 px-1 text-right">{o.px ?? '-'}</td>
            <td className="border border-gray-200 px-1 text-right">{o.qty}</td>
            <td className="border border-gray-200 px-1 text-right">{o.filled}</td>
            <td className="border border-gray-200 px-1 text-right">{o.qty - o.filled}</td>
          </tr>
        ))}
        {list.length === 0 && (
          <tr><td colSpan={7} className="text-center py-6 text-gray-400">no live orders</td></tr>
        )}
      </tbody>
    </table>
  );
};
