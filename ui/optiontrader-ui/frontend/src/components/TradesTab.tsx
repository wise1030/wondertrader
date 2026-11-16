import React from 'react';
import { useData } from '../context/DataContext';
import { OrdersTable } from './OrdersTable';

export const TradesTab: React.FC = () => {
  const { fills } = useData();
  const recent = [...fills].reverse().slice(0, 500);

  return (
    <div className="flex flex-col h-full gap-2 p-2 overflow-auto">
      <div className="bg-white border border-gray-200 rounded p-2">
        <div className="text-xs font-bold text-gray-600 mb-1">Live orders (snapshot is authoritative)</div>
        <OrdersTable />
      </div>
      <div className="bg-white border border-gray-200 rounded p-2">
        <div className="text-xs font-bold text-gray-600 mb-1">Recent fills (newest first, max 500)</div>
        <div className="grid grid-cols-2 md:grid-cols-3 lg:grid-cols-4 gap-1">
          {recent.map((f, i) => (
            <div key={i} className={`border rounded px-1.5 py-0.5 text-[10px] flex justify-between gap-2 ${f.buy ? 'border-green-200 bg-green-50' : 'border-red-200 bg-red-50'}`}>
              <span className="font-mono truncate">{f.code}</span>
              <span className={f.buy ? 'text-green-700 font-bold' : 'text-red-700 font-bold'}>{f.buy ? 'B' : 'S'}</span>
              <span>{f.qty}@{f.px}</span>
            </div>
          ))}
          {recent.length === 0 && <div className="text-gray-400 text-[11px]">no fills yet</div>}
        </div>
      </div>
    </div>
  );
};
